// 本文件由 tools/gen_ui.dart 生成，请勿手工修改。
// 定义来源：schema/ui.def.json / schema/kit.def.json；扩展 kit 定义：schema/musicxx-ui-kit.def.json
// kit（JS 插件用）：随插件目录分发，脚本里直接用全局 pluginxx.ui.kit。
//   const kit = pluginxx.ui.kit;
//   const row = kit.listRow({ title: '切歌次数', trailing: '3' }, env);
(function (global) {
  'use strict';
  var root = global.pluginxx || (global.pluginxx = {});
  var ui = root.ui || (root.ui = {});
  var kit = ui.kit || (ui.kit = {});
  kit.kitVersion = 1;
  kit.apiVersion = 1;
  kit.defaultGap = 12;
  kit.cell = { width: 8, height: 20 };

  // 组件模板：变体数组 + 参数默认值
  var TEMPLATES = {"title":{"variants":[{"template":{"kind":"Text","text":"$text","type":"title"}}],"params":{"text":null}},"hint":{"variants":[{"template":{"kind":"Text","text":"$text","type":"caption","tone":"hint"}}],"params":{"text":null}},"text":{"variants":[{"template":{"kind":"Text","text":"$text","tone":"$tone","mono":"$mono"}}],"params":{"text":null,"tone":"normal","mono":false}},"badge":{"variants":[{"template":{"kind":"Badge","text":"$text","tone":"$tone"}}],"params":{"text":null,"tone":"accent"}},"icon":{"variants":[{"requires":["Icon"],"template":{"kind":"Icon","name":"$name","glyph":"$glyph","size":"$size","tone":"$tone"}},{"template":{"kind":"Text","text":"$glyph","mono":true,"tone":"$tone"}}],"params":{"name":null,"glyph":null,"size":null,"tone":"normal"}},"gap":{"variants":[{"template":{"kind":"Gap","size":"$size"}}],"params":{"size":null}},"divider":{"variants":[{"template":{"kind":"Divider"}}],"params":{}},"button":{"variants":[{"template":{"kind":"Button","label":"$label","variant":"$variant","icon":"$icon","disabled":"$disabled","action":"$action"}}],"params":{"label":null,"variant":"secondary","icon":null,"disabled":false,"action":null}},"actionsRow":{"variants":[{"template":{"kind":"Row","gap":12,"children":{"$map":"buttons","wrap":{"kind":"Expanded","children":["$item"]}}}}],"params":{"buttons":null}},"card":{"variants":[{"template":{"kind":"Block","title":"$title","variant":"$variant","padding":"$padding","margin":"$margin","children":"$children"}}],"params":{"title":null,"variant":"card","padding":null,"margin":null,"children":null}},"listRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","tone":"hint"}]}]}}],"params":{"title":null,"subtitle":null,"trailing":null,"action":null}},"section":{"variants":[{"template":{"kind":"Column","gap":8,"children":[{"kind":"Text","text":"$title","type":"title"},{"kind":"Column","children":"$rows"}]}}],"params":{"title":null,"rows":null}},"kv":{"variants":[{"template":{"kind":"KV","pairs":"$pairs","sep":"$sep","keyWidth":"auto"}}],"params":{"pairs":null,"sep":null}},"table":{"variants":[{"template":{"kind":"Table","header":"$header","columns":"$columns","rows":"$rows"}}],"params":{"columns":null,"rows":null,"header":true}},"tree":{"variants":[{"template":{"kind":"Tree","connector":"$connector","nodes":"$nodes"}}],"params":{"nodes":null,"connector":true}},"sparkline":{"variants":[{"template":{"kind":"Sparkline","data":"$data","height":"$height","glyphStyle":"$glyphStyle","showLast":"$showLast","tone":"$tone"}}],"params":{"data":null,"height":1,"glyphStyle":"block","showLast":true,"tone":"accent"}},"progressRow":{"variants":[{"template":{"kind":"Row","gap":12,"cross":"center","children":[{"$require":"label","kind":"Text","text":"$label"},{"kind":"Expanded","children":[{"kind":"Progress","value":"$value","total":"$total","unit":"$unit"}]}]}}],"params":{"label":null,"value":null,"total":100,"unit":"%"}},"settingRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"$require":"value","kind":"Text","text":"$value","tone":"hint"}]}]}}],"params":{"title":null,"depict":null,"value":null,"action":null}},"switchRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"kind":"Control","control":"switch","id":"$id","value":"$value","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"action":null}},"inputRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Column","gap":6,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"},{"kind":"Control","control":"text","id":"$id","value":"$value","multiline":"$multiline","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"multiline":false,"action":null}},"shaderBlock":{"variants":[{"requires":["musicxx.Shader"],"template":{"kind":"musicxx.Shader","bundle":"$bundle","args":"$args","speed":"$speed","maxFps":"$maxFps","animate":"$animate","resolutionScale":"$resolutionScale"}},{"template":{"kind":"Text","text":"（这个客户端不支持插件着色器）","type":"caption","tone":"hint"}}],"params":{"bundle":null,"args":null,"speed":1,"maxFps":null,"animate":true,"resolutionScale":1}},"coverRow":{"variants":[{"requires":["Image"],"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Image","source":"file","src":"$cover","width":"$size","height":"$size","radius":4,"alt":"$title"},{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","tone":"hint"}]}]}},{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","tone":"hint"}]}]}}],"params":{"cover":null,"title":null,"subtitle":null,"trailing":null,"size":40,"action":null}},"animScope":{"variants":[{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","children":"$children"}},{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","$require":"maxFps","maxFps":"$maxFps","children":"$children"}}],"params":{"values":null,"maxFps":null,"children":null}},"fadeTransition":{"variants":[{"template":{"kind":"musicxx.FadeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.FadeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}}],"params":{"value":null,"curve":null,"children":null}},"sizeTransition":{"variants":[{"template":{"kind":"musicxx.SizeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","axis":"$axis","curve":"$curve","children":"$children"}}],"params":{"value":null,"axis":null,"curve":null,"children":null}},"scaleTransition":{"variants":[{"template":{"kind":"musicxx.ScaleTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}},"rotationTransition":{"variants":[{"template":{"kind":"musicxx.RotationTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.RotationTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}},"slideTransition":{"variants":[{"template":{"kind":"musicxx.SlideTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SlideTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}}};

  function present(v) {
    if (v === undefined || v === null) return false;
    if (typeof v === 'string') return v.length > 0;
    return true;
  }

  function isEmpty(v) {
    if (!present(v)) return true;
    return Array.isArray(v) && v.length === 0;
  }

  function pickVariant(name, env) {
    var t = TEMPLATES[name];
    if (!t) return null;
    var variants = t.variants;
    // 目标未知（没有 env）：用第一个变体（插件作者认为最合适的那一个）
    if (!env) return variants.length ? variants[0].template : null;
    var fallback = null;
    for (var i = 0; i < variants.length; i++) {
      var need = variants[i].requires || [];
      if (need.length === 0) {
        if (fallback === null) fallback = variants[i];
        continue;
      }
      var ok = true;
      for (var j = 0; j < need.length; j++) {
        if (!env.blocks || env.blocks.indexOf(need[j]) < 0) { ok = false; break; }
      }
      if (ok) return variants[i].template;
    }
    var chosen = fallback !== null ? fallback : variants[0];
    return chosen ? chosen.template : null;
  }

  function stringify(v) {
    if (v === undefined || v === null) return '';
    if (typeof v === 'string') return v;
    if (typeof v === 'number' || typeof v === 'boolean') return String(v);
    return JSON.stringify(v);
  }

  function interpolate(text, values) {
    return text.replace(/\$\{([A-Za-z0-9_]+)\}/g, function (_, name) {
      return stringify(values[name]);
    });
  }

  function expandMap(node, values) {
    var out = [];
    if (node === null || typeof node !== 'object' || Array.isArray(node) || node.$map === undefined) {
      var single = expandNode(node, values);
      if (single !== undefined) out.push(single);
      return out;
    }
    var list = values[node.$map];
    if (!Array.isArray(list)) return out;
    for (var i = 0; i < list.length; i++) {
      var scope = {};
      for (var key in values) scope[key] = values[key];
      scope.item = list[i];
      var item = expandNode(node.wrap, scope);
      if (item !== undefined) out.push(item);
    }
    return out;
  }

  function expandNode(node, values) {
    if (typeof node === 'string') {
      if (node.length > 1 && node.charAt(0) === '$' && node.indexOf('{') < 0) {
        var value = values[node.substring(1)];
        return value === undefined ? null : value;
      }
      return node.indexOf('${') >= 0 ? interpolate(node, values) : node;
    }
    if (Array.isArray(node)) {
      var list = [];
      for (var i = 0; i < node.length; i++) list = list.concat(expandMap(node[i], values));
      return list;
    }
    if (node === null || typeof node !== 'object') return node;
    if (node.$map !== undefined) return expandMap(node, values);
    if (node.$require !== undefined && isEmpty(values[node.$require])) return undefined;
    var obj = {};
    for (var key in node) {
      if (key === '$require') continue;
      var child = expandNode(node[key], values);
      if (child === undefined || child === null) continue;
      obj[key] = child;
    }
    return obj;
  }

  function expand(name, params, env) {
    var template = pickVariant(name, env);
    if (template === null) return null;
    var declared = (TEMPLATES[name] || {}).params || {};
    var values = {};
    for (var key in declared) {
      values[key] = params[key] === undefined ? declared[key] : params[key];
    }
    for (var given in params) {
      if (values[given] === undefined) values[given] = params[given];
    }
    var result = expandNode(template, values);
    return result === undefined ? null : result;
  }

  kit.expand = expand;


  // 按格换算成 u（n 列）
  kit.cols = function (count, env) {
    return count * ((env && env.cell && env.cell.width) || kit.cell.width);
  };
  // 按格换算成 u（n 行）
  kit.rows = function (count, env) {
    return count * ((env && env.cell && env.cell.height) || kit.cell.height);
  };

  // 标题行
  // 参数：text(text, 必填)
  kit.title = function (params, env) { return expand("title", params || {}, env); };

  // 次要说明行
  // 参数：text(text, 必填)
  kit.hint = function (params, env) { return expand("hint", params || {}, env); };

  // 正文行
  // 参数：text(text, 必填)、tone(tone, 默认 normal)、mono(bool, 默认 false)
  kit.text = function (params, env) { return expand("text", params || {}, env); };

  // 状态小标签
  // 参数：text(text, 必填)、tone(tone, 默认 accent)
  kit.badge = function (params, env) { return expand("badge", params || {}, env); };

  // 图标（目标不支持 Icon 时退化成 glyph 文本）
  // 参数：name(string)、glyph(string)、size(size)、tone(tone, 默认 normal)
  kit.icon = function (params, env) { return expand("icon", params || {}, env); };

  // 竖直留白（缺省用客户端默认行距）
  // 参数：size(size, 默认 gap)
  kit.gap = function (params, env) { return expand("gap", params || {}, env); };

  // 分隔线
  // 参数：
  kit.divider = function (params, env) { return expand("divider", params || {}, env); };

  // 按钮
  // 参数：label(text, 必填)、variant(enum:buttonVariant, 默认 secondary)、icon(string)、disabled(bool, 默认 false)、action(action)
  kit.button = function (params, env) { return expand("button", params || {}, env); };

  // 一排等宽按钮（按钮列表里的每一项占一等份）
  // 参数：buttons(items, 必填)
  kit.actionsRow = function (params, env) { return expand("actionsRow", params || {}, env); };

  // 内容块（卡片）
  // 参数：title(text)、variant(enum:blockVariant, 默认 card)、padding(edges)、margin(edges)、children(items)
  kit.card = function (params, env) { return expand("card", params || {}, env); };

  // 卡片里的一行（覆盖基础 kit：musicxx 的卡片行留白更紧，副标题用说明样式）
  // 参数：title(text, 必填)、subtitle(text)、trailing(text)、action(action)
  kit.listRow = function (params, env) { return expand("listRow", params || {}, env); };

  // 小节标题 + 若干行
  // 参数：title(text, 必填)、rows(items, 必填)
  kit.section = function (params, env) { return expand("section", params || {}, env); };

  // 键值块（键列按最长键自适应）
  // 参数：pairs(pairs, 必填)、sep(string)
  kit.kv = function (params, env) { return expand("kv", params || {}, env); };

  // 表格（未指定的列宽由客户端自动分配）
  // 参数：columns(columns, 必填)、rows(rows)、header(bool, 默认 true)
  kit.table = function (params, env) { return expand("table", params || {}, env); };

  // 层级列表
  // 参数：nodes(nodes, 必填)、connector(bool, 默认 true)
  kit.tree = function (params, env) { return expand("tree", params || {}, env); };

  // 迷你趋势图
  // 参数：data(numbers, 必填)、height(int, 默认 1)、glyphStyle(enum:sparkStyle, 默认 block)、showLast(bool, 默认 true)、tone(tone, 默认 accent)
  kit.sparkline = function (params, env) { return expand("sparkline", params || {}, env); };

  // 一行进度（覆盖基础 kit：musicxx 默认按百分比显示数值）
  // 参数：label(text)、value(float, 必填)、total(float, 默认 100)、unit(string, 默认 %)
  kit.progressRow = function (params, env) { return expand("progressRow", params || {}, env); };

  // 设置行：标题 + 说明 + 右侧当前值（整行可点，点开会话由插件给的动作处理）
  // 参数：title(text, 必填)、depict(text)、value(text)、action(action)
  kit.settingRow = function (params, env) { return expand("settingRow", params || {}, env); };

  // 开关行：标题 + 说明 + 右侧开关（值变化即派发动作，参数里带 id 与当前值）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、action(action)
  kit.switchRow = function (params, env) { return expand("switchRow", params || {}, env); };

  // 输入行：标题 + 说明 + 右侧单行/多行输入框（输入完成即派发动作）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、multiline(bool, 默认 false)、action(action)
  kit.inputRow = function (params, env) { return expand("inputRow", params || {}, env); };

  // 插件着色器块（musicxx 专属组件；终端之类不认识它的客户端会跳过）
  // 参数：bundle(string, 必填)、args(json)、speed(float, 默认 1)、maxFps(int)、animate(bool, 默认 true)、resolutionScale(float, 默认 1)
  kit.shaderBlock = function (params, env) { return expand("shaderBlock", params || {}, env); };

  // 带封面的行：封面 + 标题 + 副标题 + 可选右侧文字（取不到图片时显示标题）
  // 参数：cover(string, 必填)、title(text, 必填)、subtitle(text)、trailing(text)、size(size, 默认 40)、action(action)
  kit.coverRow = function (params, env) { return expand("coverRow", params || {}, env); };

  // 动画作用域：声明通道表（名字 → 值表达式）并逐帧驱动子树（musicxx.AnimatedBuilder）
  // 参数：values(json, 必填)、maxFps(int)、children(items, 必填)
  kit.animScope = function (params, env) { return expand("animScope", params || {}, env); };

  // 透明度过渡：按 value（0~1，字面量或值表达式）淡入淡出；curve 是简写缓动
  // 参数：value(value, 必填)、curve(enum:ease)、children(items, 必填)
  kit.fadeTransition = function (params, env) { return expand("fadeTransition", params || {}, env); };

  // 尺寸过渡：按 value（0~1）把子块从 0 撑开 / 收拢（axis 缺省竖直）
  // 参数：value(value, 必填)、axis(enum:axis)、curve(enum:ease)、children(items, 必填)
  kit.sizeTransition = function (params, env) { return expand("sizeTransition", params || {}, env); };

  // 缩放过渡：按 value（0~1）从 from 缩放到 to（缺省 0 → 1）
  // 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
  kit.scaleTransition = function (params, env) { return expand("scaleTransition", params || {}, env); };

  // 旋转过渡：按 value（0~1）从 from 转到 to（单位 = 圈数，缺省 0 → 1）
  // 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
  kit.rotationTransition = function (params, env) { return expand("rotationTransition", params || {}, env); };

  // 位移过渡：按 value（0~1）从 from 移到 to（单位 = 自身尺寸的倍数，如 [0, 0.1] → [0, 0]）
  // 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
  kit.slideTransition = function (params, env) { return expand("slideTransition", params || {}, env); };
})(
  typeof globalThis !== 'undefined' ? globalThis : this
);
