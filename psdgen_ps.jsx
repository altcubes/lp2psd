/*
 * psdgen_ps.jsx — PSD 排版生成 + 字体/字号设置 + 文本图层更新（Photoshop 脚本）
 *
 * 功能：
 *   1. 选择排版文本文件（.txt）
 *   2. 读取 config.json 中的字体与文字大小；可从本机所有字体中选择字体、
 *      修改文字大小，并写回 config.json（与 psdgen.exe 默认读取位置一致）
 *   3. 以批处理方式调用 psdgen.exe 生成 PSD（输出到文本文件目录下的 output）
 *   4. 遍历所有生成的 PSD：打开 → 播放录制的“更新所有文字图层”动作 → 保存 → 关闭
 *
 * 用法：Photoshop 菜单 文件 > 脚本 > 浏览... 选择本文件运行。
 * 注意：本文件必须保持 UTF-8（含 BOM）编码，否则中文界面会乱码。
 *       psdgen.exe 与 config.json 应放在脚本同目录（或同目录的 build 子目录），
 *       找不到时会弹出选择框。
 *
 * 重要（必需）：Photoshop 没有公开“更新所有文字图层”菜单命令的脚本接口，
 * 需要先把这条命令录制为一个动作（只需一次）：
 *   1. 打开一个含“需要更新版面”文本图层的 PSD；
 *   2. 窗口 > 动作，点“创建新动作”，名称填“更新所有文字图层”
 *      （放在默认的“默认动作”组里即可）；
 *   3. 点“开始录制”，执行菜单 类型 > 更新所有文字图层，然后“停止播放/记录”；
 *   4. 之后运行本脚本时，它会自动播放该动作（效果与手动点击完全一致）。
 */
#target photoshop

app.displayDialogs = DialogModes.NO;

// ============================ 工具函数 ============================

function trim(s) {
    return String(s).replace(/^\s+|\s+$/g, "");
}

function readUTF8(path) {
    try {
        var f = new File(path);
        if (!f.exists) return null;
        if (!f.open("r", "TEXT", "UTF-8")) return null;
        var t = f.read();
        f.close();
        if (t.length > 0 && t.charCodeAt(0) === 0xFEFF) t = t.substr(1);
        return t;
    } catch (e) {
        return null;
    }
}

function writeUTF8(path, text) {
    try {
        var f = new File(path);
        if (!f.open("w", "TEXT", "UTF-8")) return false;
        f.write(text);
        f.close();
        return true;
    } catch (e) {
        return false;
    }
}

function parseJSON(text) {
    if (!text) return null;
    try {
        return eval("(" + text + ")");
    } catch (e) {
        return null;
    }
}

function jsonEscape(s) {
    s = String(s);
    var out = "", i, c, cc, h;
    for (i = 0; i < s.length; i++) {
        c = s.charAt(i);
        cc = s.charCodeAt(i);
        if (c === '"') out += '\\"';
        else if (c === "\\") out += "\\\\";
        else if (c === "\n") out += "\\n";
        else if (c === "\r") out += "\\r";
        else if (c === "\t") out += "\\t";
        else if (cc < 0x20) {
            h = cc.toString(16);
            while (h.length < 4) h = "0" + h;
            out += "\\u" + h;
        } else {
            out += c;
        }
    }
    return '"' + out + '"';
}

function jsonStringify(v, indent) {
    indent = indent || 0;
    var pad = "", i;
    for (i = 0; i < indent; i++) pad += "  ";
    if (v === null || v === undefined) return "null";
    var t = typeof v;
    if (t === "number") return isFinite(v) ? String(v) : "null";
    if (t === "boolean") return v ? "true" : "false";
    if (t === "string") return jsonEscape(v);
    if (v instanceof Array) {
        var arr = [];
        for (i = 0; i < v.length; i++) arr.push(jsonStringify(v[i], indent + 1));
        if (arr.length === 0) return "[]";
        return "[\n" + pad + "  " + arr.join(",\n" + pad + "  ") + "\n" + pad + "]";
    }
    if (t === "object") {
        var parts = [];
        for (var k in v) {
            if (!v.hasOwnProperty(k)) continue;
            parts.push(pad + "  " + jsonEscape(k) + ": " + jsonStringify(v[k], indent + 1));
        }
        if (parts.length === 0) return "{}";
        return "{\n" + parts.join(",\n") + "\n" + pad + "}";
    }
    return "null";
}

// 列出 Photoshop 中可用的全部本地字体（按 PostScript 名去重、按字体族排序）
function listLocalFonts() {
    var out = [];
    try {
        var n = app.fonts.length;
        for (var i = 0; i < n; i++) {
            var f = app.fonts[i];
            if (!f) continue;
            var ps = "", fam = "", st = "";
            try { ps = f.postScriptName; } catch (e) {}
            try { fam = f.family; } catch (e) {}
            try { st = f.styleName; } catch (e) {}
            if (!ps) { try { ps = f.name; } catch (e) {} }
            if (!fam) fam = ps;
            if (!ps) continue;
            out.push({ ps: String(ps), family: String(fam), style: String(st) });
        }
    } catch (e) {
        return null;
    }
    if (out.length === 0) return null;
    var seen = {}, unique = [], j, key;
    for (j = 0; j < out.length; j++) {
        key = out[j].ps.toLowerCase();
        if (seen[key]) continue;
        seen[key] = 1;
        unique.push(out[j]);
    }
    unique.sort(function (a, b) {
        var r = a.family < b.family ? -1 : (a.family > b.family ? 1 : 0);
        if (r !== 0) return r;
        return a.style < b.style ? -1 : (a.style > b.style ? 1 : 0);
    });
    return unique;
}

// 播放用户录制的“更新所有文字图层”动作（效果与手动点击菜单一致）。
// 找到并成功播放返回 true，否则返回 false。
function playUpdateAction() {
    var actionNames = ["更新所有文字图层", "更新文字图层", "UpdateAllTextLayers", "Update All Text Layers"];
    var actionSets = ["psdgen", "默认动作", "Default Actions"];
    for (var s2 = 0; s2 < actionSets.length; s2++) {
        for (var n2 = 0; n2 < actionNames.length; n2++) {
            try {
                app.doAction(actionNames[n2], actionSets[s2]);
                return true;
            } catch (e) {}
        }
    }
    return false;
}

// 统计文档中的文本图层数量（含分组内），用于结果提示
function countTextLayers(doc) {
    var n = 0;
    function walk(ls) {
        for (var i = 0; i < ls.length; i++) {
            var l = ls[i];
            var isGroup = false;
            try { isGroup = (l.typename === "LayerSet"); } catch (e) {}
            if (isGroup) {
                try { walk(l.layers); } catch (e) {}
                continue;
            }
            try {
                if (l.kind === LayerKind.TEXT) n++;
            } catch (e) {}
        }
    }
    walk(doc.layers);
    return n;
}

// ============================ 主对话框 ============================

function showDialog(state) {
    var win = new Window("dialog", "PSD 排版生成与文本图层更新");
    win.orientation = "column";
    win.alignChildren = "fill";
    win.spacing = 8;
    win.margins = 12;

    var g1 = win.add("group");
    g1.orientation = "row";
    g1.alignChildren = "center";
    g1.add("statictext", undefined, "文本文件:");
    var etTxt = g1.add("edittext", undefined, state.txtPath);
    etTxt.preferredSize.width = 340;
    var btnTxt = g1.add("button", undefined, "浏览...");
    btnTxt.onClick = function () {
        var f = File.openDialog("选择排版文本文件", "文本文件:*.txt;*.TXT;所有文件:*.*");
        if (f) etTxt.text = f.fsName;
    };

    var g2 = win.add("group");
    g2.orientation = "row";
    g2.alignChildren = "center";
    g2.add("statictext", undefined, "配置文件:");
    var etCfg = g2.add("edittext", undefined, state.cfgPath);
    etCfg.preferredSize.width = 340;
    var btnCfg = g2.add("button", undefined, "浏览...");
    btnCfg.onClick = function () {
        var f = File.openDialog("选择 config.json", "配置文件:*.json;所有文件:*.*");
        if (f) etCfg.text = f.fsName;
    };

    var g3 = win.add("group");
    g3.orientation = "row";
    g3.alignChildren = "center";
    g3.add("statictext", undefined, "字体:");
    var dd = null, etFont = null;
    if (state.fonts) {
        var labels = [], i;
        for (i = 0; i < state.fonts.length; i++) {
            var ft = state.fonts[i];
            labels.push(ft.family + (ft.style ? " / " + ft.style : "") + "  [" + ft.ps + "]");
        }
        dd = g3.add("dropdownlist", undefined, labels);
        dd.preferredSize.width = 340;
        var sel = -1;
        for (i = 0; i < state.fonts.length; i++) {
            if (state.fonts[i].ps.toLowerCase() === String(state.curFont).toLowerCase()) {
                sel = i;
                break;
            }
        }
        if (sel < 0) {
            for (i = 0; i < state.fonts.length; i++) {
                if (String(state.fonts[i].family).toLowerCase() === String(state.curFont).toLowerCase()) {
                    sel = i;
                    break;
                }
            }
        }
        if (sel < 0) sel = 0;
        dd.selection = sel;
    } else {
        etFont = g3.add("edittext", undefined, state.curFont);
        etFont.preferredSize.width = 340;
    }

    var g4 = win.add("group");
    g4.orientation = "row";
    g4.alignChildren = "center";
    g4.add("statictext", undefined, "文字大小(点):");
    var etSize = g4.add("edittext", undefined, String(state.curSize));
    etSize.preferredSize.width = 100;

    var stNote = win.add("statictext", undefined, state.note);
    stNote.characters = 62;

    var g5 = win.add("group");
    g5.alignment = "center";
    var btnOk = g5.add("button", undefined, "生成并更新");
    var btnCancel = g5.add("button", undefined, "取消");
    btnOk.onClick = function () { win.close(1); };
    btnCancel.onClick = function () { win.close(0); };
    win.defaultElement = btnOk;
    win.cancelElement = btnCancel;

    if (win.show() !== 1) return null;

    var result = {
        txtPath: trim(etTxt.text),
        cfgPath: trim(etCfg.text),
        fontPs: dd ? state.fonts[dd.selection.index].ps : trim(etFont.text),
        size: parseFloat(trim(etSize.text))
    };
    if (isNaN(result.size) || result.size <= 0) result.size = 0;
    return result;
}

// ============================ 生成与等待 ============================

function qArg(s) { return '"' + s + '"'; }

// 等待生成完成：
//  - 15 秒内日志文件都没出现 → 认为进程没有真正启动，返回失败
//  - 日志出现后最多等 120 秒，直到出现 done:（psdgen 全部写完才打印）
function waitForGeneration(logFile, setProg) {
    var waited = 0;
    var step = 500;
    var launchWindow = 15000;
    var hardLimit = 120000;
    var sawLog = false;
    while (waited < hardLimit) {
        var logExists = false;
        try { logExists = logFile.exists; } catch (e) {}
        if (logExists) {
            sawLog = true;
            var t = readUTF8(logFile.fsName) || "";
            if (t.indexOf("done:") >= 0) return { ok: true, log: t };
            if (t.indexOf("parse error") >= 0 || t.indexOf("unknown argument") >= 0) {
                return { ok: false, log: t };
            }
        }
        if (!sawLog && waited >= launchWindow) {
            // 进程没有真正启动（重定向未创建日志）
            return { ok: false, log: "" };
        }
        if (setProg) {
            try { setProg("正在生成 PSD... " + Math.floor(waited / 1000) + " 秒"); } catch (e) {}
        }
        $.sleep(step);
        waited += step;
    }
    var tail = "";
    if (logFile.exists) tail = readUTF8(logFile.fsName) || "";
    return { ok: false, log: tail };
}

function runGenerator(exe, txtFile, cfgPath, setProg) {
    var stamp = String(new Date().getTime());
    var logFile = new File(Folder.temp.fsName + "/psdgen_run_" + stamp + ".log");
    var inner = qArg(exe.fsName) + " " + qArg(txtFile.fsName) +
                " --config " + qArg(cfgPath) +
                " > " + qArg(logFile.fsName) + " 2>&1";

    // 临时批处理文件（引号与重定向最可靠，中文路径按系统 GBK/ANSI 写入）
    var batPath = Folder.temp.fsName + "/psdgen_run_" + stamp + ".bat";
    try {
        var bf = new File(batPath);
        var bopen = bf.open("w", "TEXT", "GBK");
        if (!bopen) bopen = bf.open("w", "TEXT", "ANSI");
        if (!bopen) {
            return { ok: false, log: "无法创建临时批处理文件: " + batPath, codes: ["batch=create-fail"], logFile: logFile };
        }
        bf.write("@echo off\r\n" + inner + "\r\n");
        bf.close();
    } catch (e) {
        return { ok: false, log: "创建批处理文件异常: " + e, codes: ["batch=create-fail"], logFile: logFile };
    }

    // 用 VBS 隐藏启动 cmd（不弹出黑色窗口），wscript 等批处理结束后退出；
    // VBS 内容为纯 ASCII（临时路径无中文），无需编码转换。
    var vbsPath = Folder.temp.fsName + "/psdgen_run_" + stamp + ".vbs";
    var vbsOk = false;
    try {
        var vf = new File(vbsPath);
        if (vf.open("w", "TEXT", "ANSI")) {
            vf.write('Set sh = CreateObject("WScript.Shell")\r\n');
            vf.write('sh.Run "cmd /c ""' + batPath + '"", 0, True\r\n');
            vf.close();
            vbsOk = true;
        }
    } catch (e) {
        vbsOk = false;
    }

    var code = -1;
    if (vbsOk) {
        try { code = app.system("wscript.exe " + qArg(vbsPath)); } catch (e) { code = -1; }
    }
    if (!vbsOk || code === -1) {
        // 兜底：显示 cmd 窗口直接运行
        try { code = app.system("cmd /c " + qArg(batPath)); } catch (e) { code = -1; }
    }
    var r = waitForGeneration(logFile, setProg);
    if (r.ok) {
        return { ok: true, log: r.log || "", codes: ["batch=" + code], logFile: logFile, label: "batch" };
    }
    return { ok: false, log: r.log || "", codes: ["batch=" + code], logFile: logFile, cmd: "cmd /c " + qArg(batPath) };
}

// ============================ 主流程 ============================

function main() {
    var scriptDir = Folder($.fileName).parent;

    // ---- 定位 psdgen.exe：脚本目录 → build 子目录 → 手动选择 ----
    var exeCandidates = [
        new File(scriptDir.fsName + "/psdgen.exe"),
        new File(scriptDir.fsName + "/build/psdgen.exe"),
        new File(scriptDir.fsName + "/build/Release/psdgen.exe")
    ];
    var exe = null, i;
    for (i = 0; i < exeCandidates.length; i++) {
        if (exeCandidates[i].exists) {
            exe = exeCandidates[i];
            break;
        }
    }
    if (!exe) {
        var picked = File.openDialog("未找到 psdgen.exe，请选择程序", "程序:psdgen.exe;*.exe");
        if (picked && picked.exists) exe = picked;
    }
    if (!exe) {
        alert("未找到 psdgen.exe，无法继续。");
        return;
    }

    // ---- 定位 config.json：优先 exe 同目录（程序默认读取位置），其次脚本目录 ----
    var cfgCandidates = [
        new File(exe.parent.fsName + "/config.json"),
        new File(scriptDir.fsName + "/config.json")
    ];
    var cfgFile = null, j;
    for (j = 0; j < cfgCandidates.length; j++) {
        if (cfgCandidates[j].exists) {
            cfgFile = cfgCandidates[j];
            break;
        }
    }

    // ---- 读取当前配置中的字体与字号 ----
    var curFont = "Microsoft YaHei", curSize = 24, cfgObj = null, cfgText = null;
    if (cfgFile) cfgText = readUTF8(cfgFile.fsName);
    if (cfgText) {
        cfgObj = parseJSON(cfgText);
        if (cfgObj && cfgObj.font) {
            if (cfgObj.font.name) curFont = String(cfgObj.font.name);
            if (cfgObj.font.fontSize !== undefined) curSize = parseFloat(cfgObj.font.fontSize);
        }
    }
    if (isNaN(curSize) || curSize <= 0) curSize = 24;

    var fonts = listLocalFonts();
    var note = cfgFile
        ? "配置文件: " + cfgFile.fsName
        : "config.json 不存在，将按下方设置创建: " + cfgCandidates[0].fsName;

    var state = {
        txtPath: "",
        cfgPath: cfgFile ? cfgFile.fsName : cfgCandidates[0].fsName,
        curFont: curFont,
        curSize: curSize,
        fonts: fonts,
        note: note
    };

    var res = showDialog(state);
    if (!res) return;

    // ---- 校验 ----
    var txtFile = new File(res.txtPath);
    if (!txtFile.exists) {
        alert("文本文件不存在:\n" + res.txtPath);
        return;
    }
    if (res.size <= 0) {
        alert("请输入有效的文字大小（点）。");
        return;
    }
    if (!res.fontPs) {
        alert("请输入或选择字体。");
        return;
    }

    // ---- 写回配置文件（保留其它所有字段；不存在则创建）----
    var cfgPath = res.cfgPath;
    if (!cfgPath) {
        alert("配置文件路径为空。");
        return;
    }
    if (!cfgFile || cfgPath !== cfgFile.fsName) {
        // 用户手动选择了其它配置文件，重新读取
        var t2 = readUTF8(cfgPath);
        var o2 = t2 ? parseJSON(t2) : null;
        cfgObj = o2 || { font: {} };
    }
    cfgObj = cfgObj || { font: {} };
    cfgObj.font = cfgObj.font || {};
    cfgObj.font.name = res.fontPs;
    cfgObj.font.fontSize = res.size;
    if (!writeUTF8(cfgPath, jsonStringify(cfgObj))) {
        alert("写入配置文件失败:\n" + cfgPath);
        return;
    }

    var prog = null;
    function setProg(s) {
        if (prog) {
            try {
                prog.st.text = s;
                prog.update();
            } catch (e) {}
        }
    }

    // ---- 调用 psdgen.exe 生成 PSD，并等待完成 ----
    var outDir = new Folder(txtFile.parent.fsName + "/output");
    var gen = runGenerator(exe, txtFile, cfgPath, setProg);

    // ---- 定位输出目录 ----
    var psds = [];
    if (outDir.exists) {
        try { psds = outDir.getFiles("*.psd"); } catch (e) { psds = []; }
    }
    var logText = gen.log || "";
    if (!gen.ok || psds.length === 0) {
        var msg2 = "生成失败：未找到输出目录或没有 PSD 文件\n" +
                   "输出目录: " + outDir.fsName + "\n\n" +
                   "执行方式/退出码: " + gen.codes.join(", ") + "\n\n" +
                   (gen.cmd ? "命令: " + gen.cmd + "\n\n" : "") +
                   "日志:\n" + (logText ? logText : "(无日志内容)") +
                   "\n\n如果上面有“命令:”，可复制其内容到 cmd 中手动运行排查。";
        if (prog) { try { prog.close(); } catch (e) {} }
        alert(msg2);
        return;
    }

    // ---- 进度窗口（处理 PSD 阶段显示；生成阶段不弹窗，保证立即启动）----
    try {
        prog = new Window("palette", "正在处理 PSD...");
        prog.orientation = "column";
        prog.st = prog.add("statictext", undefined, "准备中...");
        prog.st.characters = 64;
        prog.visible = true;
        prog.center();
    } catch (e) {
        prog = null;
    }

    // ---- 遍历 PSD：播放更新动作并保存 ----
    var ok = 0, fail = 0, textTotal = 0, saveFail = 0, actionDocs = 0, p;
    for (p = 0; p < psds.length; p++) {
        setProg("正在处理 (" + (p + 1) + "/" + psds.length + "): " + psds[p].name);
        try {
            var doc = app.open(psds[p]);
            if (!doc) {
                fail++;
                continue;
            }
            textTotal += countTextLayers(doc);
            var played = playUpdateAction();
            if (played) actionDocs++;
            var saved = false;
            try {
                doc.saveAs(psds[p], new PhotoshopSaveOptions(), false);
                saved = true;
            } catch (e2) {
                try { doc.save(); saved = true; } catch (e3) { saved = false; }
            }
            if (!saved) saveFail++;
            doc.close(SaveOptions.DONOTSAVECHANGES);
            ok++;
        } catch (e) {
            fail++;
        }
    }
    if (prog) {
        try { prog.close(); } catch (e) {}
    }

    var msg = "完成！\n\n" +
              "生成 PSD: " + psds.length + " 个\n" +
              "处理成功: " + ok + " 个\n" +
              "失败: " + fail + " 个\n" +
              "文本图层合计: " + textTotal + " 个\n" +
              "已播放更新动作: " + actionDocs + " 个文件" +
              (gen.label ? "\n生成调用方式: " + gen.label : "") +
              (saveFail > 0 ? "\n保存失败: " + saveFail + " 个" : "") +
              "\n\n输出目录:\n" + outDir.fsName +
              (actionDocs === 0
                  ? "\n\n提示：未找到录制的“更新所有文字图层”动作，文本图层未更新。\n" +
                    "请先录制：窗口 > 动作 > 新建动作，名称填“更新所有文字图层”（放“默认动作”组）\n" +
                    "> 点“开始录制”> 菜单 类型 > 更新所有文字图层 > 停止录制，然后重新运行本脚本。"
                  : "");
    alert(msg);
}

main();
