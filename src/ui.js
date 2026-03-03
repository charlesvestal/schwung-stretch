/*
 * Stretch Tool UI
 *
 * Standard menu-driven UI following Move Anything shared menu patterns.
 * Jog turn scrolls, jog click enters edit / triggers actions, back exits.
 *
 * Display: 128x64 1-bit
 */

import * as os from 'os';

import {
    buildFilepathBrowserState,
    refreshFilepathBrowser,
    moveFilepathBrowserSelection,
    activateFilepathBrowserItem
} from '/data/UserData/move-anything/shared/filepath_browser.mjs';

import {
    MovePad1, MovePad32,
    MoveMainButton, MoveMainKnob,
    MoveBack,
    MoveKnob1, MoveKnob8
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    setLED,
    decodeDelta,
    shouldFilterMessage
} from '/data/UserData/move-anything/shared/input_filter.mjs';

/* ========== Layout constants (match shared/menu_layout.mjs) ========== */
var SCREEN_W = 128;
var SCREEN_H = 64;
var TITLE_Y = 2;
var TITLE_RULE_Y = 12;
var FOOTER_RULE_Y = 55;
var FOOTER_TEXT_Y = 57;
var CHAR_W = 6;

/* Custom: subtitle between header and list */
var SUBTITLE_Y = 15;
var LIST_TOP_Y = 26;
var LIST_LINE_H = 9;

/* ========== LED constants ========== */
var LED_DIM_WHITE = 1;
var LED_WHITE     = 127;
var LEDS_PER_FRAME = 8;

/* ========== Bars options ========== */
var barsValues = [0.25, 0.5, 1, 2, 4, 8, 16];
var barsLabels = ["1/4", "1/2", "1", "2", "4", "8", "16"];

/* ========== Views ========== */
var VIEW_MAIN = 0;
var VIEW_SAVE_PROMPT = 1;
var VIEW_SAVE_BROWSE = 2;
var VIEW_SAVING = 3;
var VIEW_SAVED = 4;

/* ========== Menu item types ========== */
var TYPE_VALUE = "value";
var TYPE_ENUM = "enum";
var TYPE_ACTION = "action";

/* ========== Filesystem adapter for filepath_browser ========== */
var FILEPATH_BROWSER_FS = {
    readdir: function(path) {
        var out = os.readdir(path) || [];
        if (Array.isArray(out[0])) return out[0];
        if (Array.isArray(out)) return out;
        return [];
    },
    stat: function(path) {
        return os.stat(path);
    }
};

function isDirectory(path) {
    try {
        var st = os.stat(path);
        if (Array.isArray(st)) {
            var obj = st[0];
            if (obj && typeof obj.mode === "number") {
                return (obj.mode & 0o170000) === 0o040000;
            }
        }
        return false;
    } catch (e) { return false; }
}

function pathBasename(path) {
    var idx = path.lastIndexOf("/");
    return idx >= 0 ? path.slice(idx + 1) : path;
}

function pathDirname(path) {
    var idx = path.lastIndexOf("/");
    if (idx <= 0) return "/";
    return path.slice(0, idx);
}

/* ========== State ========== */
var currentView = VIEW_MAIN;

/* DSP state */
var targetBpm = 120;
var barsIndex = 2;
var pitchSemitones = 0;
var playing = false;
var fileName = "";
var sourceDuration = 0;
var speed = 1.0;
var playPos = 0;

/* Menu state */
var menuItems = [];
var selectedIndex = 0;
var editing = false;
var editValue = null;

/* Save state */
var saveChoice = 0;
var saveResult = "";
var exitTimer = 0;
var saveDestDir = "";
var saveParamQueue = [];

/* Destination directory browser */
var destBrowserState = null;
var DEST_BROWSER_ROOT = "/data/UserData/UserLibrary/Samples";

/* Input accumulation */
var pendingJogDelta = 0;

/* LEDs */
var ledInitPending = true;
var ledInitIndex = 0;

/* ========== Helpers ========== */

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

function truncStr(s, max) {
    if (!s) return "";
    return s.length <= max ? s : s.substring(0, max - 2) + "..";
}

function getSourceBpm() {
    if (sourceDuration <= 0) return 0;
    return Math.round(barsValues[barsIndex] * 4.0 * 60.0 / sourceDuration);
}

function getStem(path) {
    var name = path;
    var slash = name.lastIndexOf("/");
    if (slash >= 0) name = name.substring(slash + 1);
    var dot = name.lastIndexOf(".");
    if (dot > 0) name = name.substring(0, dot);
    return name;
}

function formatPitch(st) {
    if (st > 0) return "+" + st + " st";
    if (st < 0) return st + " st";
    return "0 st";
}

function getOutFileName() {
    var suffix = pitchSemitones !== 0
        ? "-" + targetBpm + "bpm" + (pitchSemitones > 0 ? "+" : "") + pitchSemitones + "st.wav"
        : "-" + targetBpm + "bpm.wav";
    return getStem(fileName) + suffix;
}

/* ========== Menu items ========== */

function buildMenuItems() {
    menuItems = [
        {
            type: TYPE_ENUM,
            label: "Bars",
            options: barsLabels,
            get: function() { return barsLabels[barsIndex]; },
            set: function(v) {
                var idx = barsLabels.indexOf(v);
                if (idx >= 0) {
                    barsIndex = idx;
                    sendBars();
                    readSpeed();
                }
            }
        },
        {
            type: TYPE_VALUE,
            label: "Target BPM",
            min: 40,
            max: 300,
            step: 1,
            get: function() { return targetBpm; },
            set: function(v) {
                targetBpm = v;
                sendBpm();
                readSpeed();
            }
        },
        {
            type: TYPE_VALUE,
            label: "Pitch",
            min: -12,
            max: 12,
            step: 1,
            get: function() { return pitchSemitones; },
            set: function(v) {
                pitchSemitones = v;
                sendPitch();
            }
        },
        {
            type: TYPE_ACTION,
            label: "Save...",
            onAction: function() {
                if (fileName) {
                    saveChoice = 0;
                    currentView = VIEW_SAVE_PROMPT;
                }
            }
        }
    ];
}

/* ========== DSP communication ========== */

function readDspState() {
    var v;
    v = host_module_get_param("file_name");
    if (v) fileName = v;

    v = host_module_get_param("source_duration");
    if (v) sourceDuration = parseFloat(v);

    v = host_module_get_param("target_bpm");
    if (v) targetBpm = parseInt(v, 10);

    v = host_module_get_param("target_bars");
    if (v) barsIndex = clamp(parseInt(v, 10) || 0, 0, barsValues.length - 1);

    v = host_module_get_param("pitch_semitones");
    if (v) pitchSemitones = clamp(parseInt(v, 10) || 0, -12, 12);

    v = host_module_get_param("playing");
    if (v) playing = (v === "1");

    readSpeed();
}

function readSpeed() {
    var v = host_module_get_param("speed");
    if (v) speed = parseFloat(v);
}

function readPlayState() {
    var v = host_module_get_param("playing");
    if (v) playing = (v === "1");
    v = host_module_get_param("play_pos");
    if (v) playPos = parseFloat(v);
}

function sendBpm() { host_module_set_param("target_bpm", String(targetBpm)); }
function sendBars() { host_module_set_param("target_bars", String(barsIndex)); }
function sendPitch() { host_module_set_param("pitch_semitones", String(pitchSemitones)); }
function sendPlaying(v) { host_module_set_param("playing", v ? "1" : "0"); }

/* ========== Destination directory browser ========== */

function openDestBrowser() {
    destBrowserState = buildFilepathBrowserState(
        { root: DEST_BROWSER_ROOT, filter: "__dirs_only__", name: "Save to" },
        ""
    );
    refreshDestBrowser();
    currentView = VIEW_SAVE_BROWSE;
}

function refreshDestBrowser() {
    if (!destBrowserState) return;
    var fs = FILEPATH_BROWSER_FS;
    var state = destBrowserState;
    var currentDir = state.currentDir;

    state.items = [];
    state.error = "";

    /* "Save here" option */
    state.items.push({ kind: "select_here", label: "> Save Here", path: currentDir });

    /* Parent directory navigation */
    if (currentDir !== state.root) {
        var parent = pathDirname(currentDir);
        if (parent.indexOf(state.root) === 0 || parent === state.root) {
            state.items.push({ kind: "up", label: "..", path: parent });
        }
    }

    try {
        var names = fs.readdir(currentDir) || [];
        var dirs = [];
        for (var i = 0; i < names.length; i++) {
            var name = names[i];
            if (!name || name === "." || name === "..") continue;
            if (name.startsWith(".")) continue;
            var fullPath = currentDir + "/" + name;
            if (isDirectory(fullPath)) {
                dirs.push({ kind: "dir", label: "[" + name + "]", path: fullPath });
            }
        }
        dirs.sort(function(a, b) { return a.label.localeCompare(b.label); });
        for (var j = 0; j < dirs.length; j++) {
            state.items.push(dirs[j]);
        }
    } catch (e) {
        state.error = "Unable to read folder";
    }

    if (state.selectedIndex >= state.items.length) {
        state.selectedIndex = Math.max(0, state.items.length - 1);
    }
    state.currentDir = currentDir;
}

function destBrowserNavigate(delta) {
    if (!destBrowserState || destBrowserState.items.length === 0) return;
    var dir = delta > 0 ? 1 : -1;
    destBrowserState.selectedIndex = clamp(
        destBrowserState.selectedIndex + dir,
        0, destBrowserState.items.length - 1
    );
}

function destBrowserSelect() {
    if (!destBrowserState) return;
    var sel = destBrowserState.items[destBrowserState.selectedIndex];
    if (!sel) {
        /* No items — use current dir */
        startSave(1, destBrowserState.currentDir);
        return;
    }

    if (sel.kind === "select_here") {
        startSave(1, sel.path);
    } else if (sel.kind === "up" || sel.kind === "dir") {
        destBrowserState.currentDir = sel.path;
        destBrowserState.selectedIndex = 0;
        refreshDestBrowser();
    }
}

function destBrowserBack() {
    if (!destBrowserState || destBrowserState.currentDir === destBrowserState.root) {
        currentView = VIEW_SAVE_PROMPT;
        return;
    }
    var parent = pathDirname(destBrowserState.currentDir);
    if (parent.length < destBrowserState.root.length) {
        parent = destBrowserState.root;
    }
    destBrowserState.currentDir = parent;
    destBrowserState.selectedIndex = 0;
    refreshDestBrowser();
}

/* ========== Save trigger ========== */

function startSave(mode, destDir) {
    /* Stop playback */
    if (playing) {
        playing = false;
        updatePadLEDs();
    }
    saveChoice = mode;
    saveDestDir = destDir || "";
    currentView = VIEW_SAVING;

    /* Queue params one-per-tick: in overtake mode, shadow_set_param is
     * fire-and-forget so back-to-back calls overwrite the single shared
     * memory slot before the shim can process the first one. */
    saveParamQueue = [["playing", "0"]];
    if (destDir) saveParamQueue.push(["save_dir", destDir]);
    saveParamQueue.push(["save_mode", String(mode)]);
    saveParamQueue.push(["save_result", ""]);
    saveParamQueue.push(["save", "1"]);
}

/* ========== Standard menu drawing (matches shared/menu_layout.mjs) ========== */

function drawHeader(title) {
    print(2, TITLE_Y, title, 1);
    fill_rect(0, TITLE_RULE_Y, SCREEN_W, 1, 1);
}

function drawFooter(left, right) {
    fill_rect(0, FOOTER_RULE_Y, SCREEN_W, 1, 1);
    if (left) print(2, FOOTER_TEXT_Y, left, 1);
    if (right) {
        var rw = right.length * CHAR_W;
        print(SCREEN_W - rw - 2, FOOTER_TEXT_Y, right, 1);
    }
}

function drawMenuList(items, selIdx, isEditing) {
    var maxVisible = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / LIST_LINE_H);
    var startIdx = 0;
    if (selIdx > maxVisible - 2) startIdx = selIdx - (maxVisible - 2);
    var endIdx = Math.min(startIdx + maxVisible, items.length);

    for (var i = startIdx; i < endIdx; i++) {
        var y = LIST_TOP_Y + (i - startIdx) * LIST_LINE_H;
        var item = items[i];
        var isSel = (i === selIdx);

        var prefix = isSel ? "> " : "  ";
        var label = prefix + item.label;

        /* Get display value */
        var val = "";
        if (item.type === TYPE_VALUE || item.type === TYPE_ENUM) {
            if (isEditing && isSel && editValue !== null) {
                val = String(editValue);
            } else if (item.get) {
                val = String(item.get());
            }
        }

        if (isSel) {
            fill_rect(0, y - 1, SCREEN_W, LIST_LINE_H, 1);
            print(4, y, label, 0);
            if (val) {
                var displayVal = isEditing ? ("[" + val + "]") : val;
                var vx = SCREEN_W - displayVal.length * CHAR_W - 2;
                print(vx, y, displayVal, 0);
            }
        } else {
            print(4, y, label, 1);
            if (val) {
                var vx2 = SCREEN_W - val.length * CHAR_W - 2;
                print(vx2, y, val, 1);
            }
        }
    }
}

/* Simple item list for browser (no value column) */
function drawBrowserList(items, selIdx, topY) {
    var lineH = 9;
    var maxVisible = Math.floor((FOOTER_RULE_Y - topY) / lineH);
    var startIdx = 0;
    if (selIdx > maxVisible - 2) startIdx = selIdx - (maxVisible - 2);
    var endIdx = Math.min(startIdx + maxVisible, items.length);

    for (var i = startIdx; i < endIdx; i++) {
        var y = topY + (i - startIdx) * lineH;
        var isSel = (i === selIdx);
        var label = items[i].label || "";

        if (isSel) {
            fill_rect(0, y - 1, SCREEN_W, lineH, 1);
            print(4, y, label, 0);
        } else {
            print(4, y, label, 1);
        }
    }
}

/* ========== Menu input handling (matches shared/menu_nav.mjs) ========== */

function handleJogScroll(delta) {
    if (editing) {
        var item = menuItems[selectedIndex];
        if (item.type === TYPE_VALUE) {
            var step = item.step || 1;
            editValue = clamp(editValue + delta * step, item.min, item.max);
        } else if (item.type === TYPE_ENUM) {
            var opts = item.options;
            var idx = opts.indexOf(editValue);
            var dir = delta > 0 ? 1 : -1;
            var newIdx = (idx + dir + opts.length) % opts.length;
            editValue = opts[newIdx];
        }
        if (item.set && editValue !== null) item.set(editValue);
    } else {
        var dir2 = delta > 0 ? 1 : -1;
        selectedIndex = clamp(selectedIndex + dir2, 0, menuItems.length - 1);
    }
}

function handleJogClick() {
    var item = menuItems[selectedIndex];

    if (editing) {
        editing = false;
        editValue = null;
    } else if (item.type === TYPE_VALUE || item.type === TYPE_ENUM) {
        editing = true;
        editValue = item.get ? item.get() : null;
    } else if (item.type === TYPE_ACTION) {
        if (item.onAction) item.onAction();
    }
}

function handleBack() {
    if (editing) {
        editing = false;
        editValue = null;
    } else {
        if (typeof host_exit_module === "function") host_exit_module();
    }
}

/* ========== Drawing ========== */

function drawMain() {
    var titleStr = "Stretch: " + truncStr(fileName || "(no file)", 12);
    drawHeader(titleStr);

    var srcBpm = getSourceBpm();
    var srcStr = srcBpm > 0 ? "Src: " + srcBpm + " BPM" : "Src: -- BPM";
    var rightStr = pitchSemitones !== 0
        ? formatPitch(pitchSemitones) + " " + speed.toFixed(2) + "x"
        : speed.toFixed(2) + "x";
    print(2, SUBTITLE_Y, srcStr, 1);
    print(SCREEN_W - rightStr.length * CHAR_W - 2, SUBTITLE_Y, rightStr, 1);

    drawMenuList(menuItems, selectedIndex, editing);

    if (playing) {
        drawFooter("> Playing", "Pad=Stop");
    } else {
        drawFooter("Stopped", "Pad=Play");
    }
}

function drawSavePrompt() {
    drawMain();

    var bw = 90;
    var bh = 32;
    var bx = Math.floor((SCREEN_W - bw) / 2);
    var by = Math.floor((SCREEN_H - bh) / 2);

    fill_rect(bx, by, bw, bh, 0);
    draw_rect(bx, by, bw, bh, 1);

    print(bx + 4, by + 3, "Save as...", 1);
    draw_line(bx + 1, by + 12, bx + bw - 2, by + 12, 1);

    var y0 = by + 15;
    var y1 = by + 24;

    if (saveChoice === 0) {
        fill_rect(bx + 1, y0 - 1, bw - 2, 9, 1);
        print(bx + 6, y0, "> Overwrite", 0);
        print(bx + 6, y1, "  New File", 1);
    } else {
        print(bx + 6, y0, "  Overwrite", 1);
        fill_rect(bx + 1, y1 - 1, bw - 2, 9, 1);
        print(bx + 6, y1, "> New File", 0);
    }
}

function drawSaveBrowser() {
    if (!destBrowserState) return;

    var dirName = pathBasename(destBrowserState.currentDir) || "Samples";
    drawHeader("To: " + truncStr(dirName, 15));

    if (destBrowserState.items.length === 0) {
        print(4, 24, "(no subfolders)", 1);
    } else {
        drawBrowserList(destBrowserState.items, destBrowserState.selectedIndex, 15);
    }

    /* Show output filename in footer */
    drawFooter("Back", truncStr(getOutFileName(), 16));
}

function drawSaving() {
    drawHeader("Stretch");
    print(24, 28, "Rendering...", 1);
    print(16, 40, "Please wait", 1);
}

function drawWrapped(s, x, y, maxChars, lineH) {
    while (s.length > 0) {
        var line = s.substring(0, maxChars);
        s = s.substring(maxChars);
        print(x, y, line, 1);
        y += lineH;
    }
}

function drawSaved() {
    drawHeader("Stretch");
    var maxChars = Math.floor((SCREEN_W - 8) / CHAR_W);
    if (saveResult === "ok") {
        if (saveChoice === 0) {
            print(4, 20, "Overwritten:", 1);
            drawWrapped(fileName, 4, 32, maxChars, 10);
        } else {
            var outName = getOutFileName();
            print(4, 20, "Saved to:", 1);
            drawWrapped(outName, 4, 32, maxChars, 10);
        }
    } else {
        print(4, 20, "Save failed:", 1);
        drawWrapped(saveResult, 4, 32, maxChars, 10);
    }
}

function draw() {
    clear_screen();
    if (currentView === VIEW_MAIN) drawMain();
    else if (currentView === VIEW_SAVE_PROMPT) drawSavePrompt();
    else if (currentView === VIEW_SAVE_BROWSE) drawSaveBrowser();
    else if (currentView === VIEW_SAVING) drawSaving();
    else if (currentView === VIEW_SAVED) drawSaved();
}

/* ========== MIDI handling ========== */

function handleCC(cc, val) {
    if (cc === MoveMainKnob) {
        pendingJogDelta += decodeDelta(val);
        return;
    }

    /* Knobs also adjust selected param when editing */
    if (cc >= MoveKnob1 && cc <= MoveKnob8) {
        if (currentView === VIEW_MAIN) {
            var d = decodeDelta(val);
            if (d !== 0 && !editing) {
                var item = menuItems[selectedIndex];
                if (item && (item.type === TYPE_VALUE || item.type === TYPE_ENUM)) {
                    editing = true;
                    editValue = item.get ? item.get() : null;
                }
            }
            if (editing) {
                pendingJogDelta += d;
            }
        }
        return;
    }

    if (cc === MoveMainButton && val > 0) {
        if (currentView === VIEW_MAIN) {
            handleJogClick();
        } else if (currentView === VIEW_SAVE_PROMPT) {
            if (saveChoice === 0) {
                /* Overwrite — go straight to rendering */
                startSave(0, "");
            } else {
                /* New File — open directory browser */
                openDestBrowser();
            }
        } else if (currentView === VIEW_SAVE_BROWSE) {
            destBrowserSelect();
        }
        return;
    }

    if (cc === MoveBack && val > 0) {
        if (currentView === VIEW_SAVE_PROMPT) {
            currentView = VIEW_MAIN;
        } else if (currentView === VIEW_SAVE_BROWSE) {
            destBrowserBack();
        } else if (currentView === VIEW_MAIN) {
            handleBack();
        }
        return;
    }
}

function handleNoteOn(note) {
    if (note >= MovePad1 && note <= MovePad32 && currentView === VIEW_MAIN) {
        playing = !playing;
        sendPlaying(playing);
        updatePadLEDs();
    }
}

globalThis.onMidiMessageInternal = function(data) {
    if (!data || data.length < 3) return;
    if (shouldFilterMessage(data)) return;
    if (currentView === VIEW_SAVING || currentView === VIEW_SAVED) return;

    var status = data[0] & 0xF0;
    var d1 = data[1];
    var d2 = data[2];

    if (status === 0xB0) handleCC(d1, d2);
    else if (status === 0x90 && d2 > 0) handleNoteOn(d1);
};

/* ========== LED management ========== */

function setupLedBatch() {
    var total = MovePad32 - MovePad1 + 1;
    var end = Math.min(ledInitIndex + LEDS_PER_FRAME, total);
    for (var i = ledInitIndex; i < end; i++) {
        setLED(MovePad1 + i, playing ? LED_WHITE : LED_DIM_WHITE);
    }
    ledInitIndex = end;
    if (ledInitIndex >= total) {
        ledInitPending = false;
        ledInitIndex = 0;
    }
}

function updatePadLEDs() {
    ledInitPending = true;
    ledInitIndex = 0;
}

/* ========== Lifecycle ========== */

globalThis.init = function() {
    ledInitPending = true;
    ledInitIndex = 0;
    currentView = VIEW_MAIN;
    selectedIndex = 0;
    editing = false;
    editValue = null;
    exitTimer = 0;
    pendingJogDelta = 0;

    readDspState();
    buildMenuItems();
};

globalThis.tick = function() {
    if (ledInitPending) {
        setupLedBatch();
        draw();
        return;
    }

    /* Apply accumulated jog input */
    if (pendingJogDelta !== 0) {
        if (currentView === VIEW_MAIN) {
            handleJogScroll(pendingJogDelta);
        } else if (currentView === VIEW_SAVE_PROMPT) {
            saveChoice = saveChoice === 0 ? 1 : 0;
        } else if (currentView === VIEW_SAVE_BROWSE) {
            destBrowserNavigate(pendingJogDelta);
        }
        pendingJogDelta = 0;
    }

    if (currentView === VIEW_MAIN) {
        readPlayState();
    } else if (currentView === VIEW_SAVING) {
        if (saveParamQueue.length > 0) {
            /* Send one queued param per tick (fire-and-forget safe) */
            var p = saveParamQueue.shift();
            host_module_set_param(p[0], p[1]);
        } else {
            /* All params sent — poll for result */
            var result = host_module_get_param("save_result");
            if (result && result !== "" && result !== "pending") {
                saveResult = result;
                currentView = VIEW_SAVED;
                exitTimer = 150;
            }
        }
    } else if (currentView === VIEW_SAVED) {
        if (exitTimer > 0) {
            exitTimer--;
            if (exitTimer <= 0) {
                currentView = VIEW_MAIN;
                saveResult = "";
            }
        }
    }

    draw();
};
