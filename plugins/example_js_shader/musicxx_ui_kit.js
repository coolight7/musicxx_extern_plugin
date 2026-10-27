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
  kit.kitVersion = 2;
  kit.apiVersion = 2;
  kit.defaultGap = 12;
  kit.cell = { width: 8, height: 20 };

  // 组件模板：变体数组 + 参数默认值
  var TEMPLATES = {"title":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","type":"title"}]}}],"params":{"text":null}},"hint":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","type":"caption","tone":"hint"}]}}],"params":{"text":null}},"text":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","tone":"$tone","mono":"$mono"}]}}],"params":{"text":null,"tone":"normal","mono":false}},"badge":{"variants":[{"template":{"kind":"Badge","text":"$text","tone":"$tone"}}],"params":{"text":null,"tone":"accent"}},"icon":{"variants":[{"requires":["Icon"],"template":{"kind":"Icon","name":"$name","glyph":"$glyph","size":"$size","tone":"$tone"}},{"template":{"kind":"Text","text":"$glyph","mono":true,"tone":"$tone"}}],"params":{"name":null,"glyph":null,"size":null,"tone":"normal"}},"gap":{"variants":[{"template":{"kind":"Gap","size":"$size"}}],"params":{"size":null}},"divider":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Divider"}]}}],"params":{}},"button":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Button","label":"$label","variant":"$variant","icon":"$icon","disabled":"$disabled","action":"$action"}]}}],"params":{"label":null,"variant":"secondary","icon":null,"disabled":false,"action":null}},"actionsRow":{"variants":[{"template":{"kind":"Row","gap":12,"children":{"$map":"buttons","wrap":{"kind":"Expanded","children":["$item"]}}}}],"params":{"buttons":null}},"card":{"variants":[{"template":{"kind":"Block","title":"$title","variant":"$variant","padding":"$padding","margin":"$margin","children":"$children"}}],"params":{"title":null,"variant":"card","padding":{"horizontal":50,"vertical":50},"margin":{"vertical":50},"children":null}},"listRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}}],"params":{"title":null,"subtitle":null,"trailing":null,"action":null}},"section":{"variants":[{"template":{"kind":"Column","gap":8,"children":[{"kind":"Text","text":"$title","type":"title"},{"kind":"Column","children":"$rows"}]}}],"params":{"title":null,"rows":null}},"kv":{"variants":[{"template":{"kind":"KV","pairs":"$pairs","sep":"$sep","keyWidth":"auto"}}],"params":{"pairs":null,"sep":null}},"table":{"variants":[{"template":{"kind":"Table","header":"$header","columns":"$columns","rows":"$rows"}}],"params":{"columns":null,"rows":null,"header":true}},"tree":{"variants":[{"template":{"kind":"Tree","connector":"$connector","nodes":"$nodes"}}],"params":{"nodes":null,"connector":true}},"listItem":{"variants":[{"template":{"id":"$id","title":"$title","subtitle":"$subtitle","trailing":"$trailing","image":"$image","icon":"$icon","tone":"$tone","selected":"$selected","action":"$action"}}],"params":{"id":null,"title":null,"subtitle":null,"trailing":null,"image":null,"icon":null,"tone":"normal","selected":false,"action":null}},"menuItem":{"variants":[{"template":{"title":"$title","icon":"$icon","action":"$action"}}],"params":{"title":null,"icon":null,"action":null}},"sparkline":{"variants":[{"template":{"kind":"Sparkline","data":"$data","height":"$height","glyphStyle":"$glyphStyle","showLast":"$showLast","tone":"$tone"}}],"params":{"data":null,"height":1,"glyphStyle":"block","showLast":true,"tone":"accent"}},"progressRow":{"variants":[{"template":{"kind":"Row","gap":12,"cross":"center","children":[{"$require":"label","kind":"Text","text":"$label"},{"kind":"Expanded","children":[{"kind":"Progress","value":"$value","total":"$total","unit":"$unit"}]}]}}],"params":{"label":null,"value":null,"total":100,"unit":"%"}},"dirListView":{"variants":[{"requires":["musicxx.DirListView"],"template":{"kind":"musicxx.DirListView","path":"$path","items":"$items","multi":"$multi","showPath":"$showPath","pathAction":"$pathAction","itemAction":"$itemAction","selectAction":"$selectAction","height":"$height","emptyText":"$emptyText"}},{"template":{"kind":"ListViewBase","id":"dirList","items":"$items","itemAction":"$itemAction","emptyText":"$emptyText"}}],"params":{"path":null,"items":null,"multi":false,"showPath":true,"pathAction":null,"itemAction":null,"selectAction":null,"height":null,"emptyText":null}},"adaptLayout":{"variants":[{"requires":["musicxx.AdaptLayout"],"template":{"kind":"musicxx.AdaptLayout","columnBody":"$columnBody","rowBody":"$rowBody","columnSecondBody":"$columnSecondBody","rowSecondBody":"$rowSecondBody","rowBodyRatio":"$rowBodyRatio","columnBodyRatio":"$columnBodyRatio","enableAnimations":"$enableAnimations"}},{"template":{"kind":"Column","children":"$rowBody"}}],"params":{"columnBody":null,"rowBody":null,"columnSecondBody":null,"rowSecondBody":null,"rowBodyRatio":0.5,"columnBodyRatio":0.5,"enableAnimations":false}},"settingRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"$require":"value","kind":"Text","text":"$value","type":"caption","tone":"hint"}]}]}}],"params":{"title":null,"depict":null,"value":null,"action":null}},"switchRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"kind":"Control","control":"switch","id":"$id","value":"$value","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"action":null}},"inputRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Column","gap":6,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"},{"kind":"Control","control":"text","id":"$id","value":"$value","multiline":"$multiline","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"multiline":false,"action":null}},"passwordRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Column","gap":6,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"},{"kind":"Control","control":"password","id":"$id","value":"$value","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"action":null}},"selectRow":{"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Column","gap":6,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"},{"kind":"Control","control":"select","id":"$id","value":"$value","options":"$options","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"options":null,"action":null}},"overlayRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":12,"vertical":8},"action":{"kind":"command","name":"musicxx.ui.overlay.show","args":{"title":"$title","depict":"$depict","blocks":"$blocks","textConfirm":"$textConfirm"}},"children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Text","text":"$label"},{"$require":"value","kind":"Text","text":"$value","type":"caption","tone":"hint"}]}]}]}}],"params":{"label":null,"value":null,"title":null,"depict":null,"blocks":null,"textConfirm":null}},"shaderBlock":{"variants":[{"requires":["musicxx.Shader"],"template":{"kind":"musicxx.Shader","bundle":"$bundle","args":"$args","speed":"$speed","maxFps":"$maxFps","animate":"$animate","resolutionScale":"$resolutionScale"}},{"template":{"kind":"Text","text":"（这个客户端不支持插件着色器）","type":"caption","tone":"hint"}}],"params":{"bundle":null,"args":null,"speed":1,"maxFps":null,"animate":true,"resolutionScale":1}},"coverRow":{"variants":[{"requires":["Image"],"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Image","source":"file","src":"$cover","width":"$size","height":"$size","radius":4,"alt":"$title"},{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}},{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}}],"params":{"cover":null,"title":null,"subtitle":null,"trailing":null,"size":40,"action":null}},"animScope":{"variants":[{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","children":"$children"}},{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","$require":"maxFps","maxFps":"$maxFps","children":"$children"}}],"params":{"values":null,"maxFps":null,"children":null}},"fadeTransition":{"variants":[{"template":{"kind":"musicxx.FadeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.FadeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}}],"params":{"value":null,"curve":null,"children":null}},"sizeTransition":{"variants":[{"template":{"kind":"musicxx.SizeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","axis":"$axis","curve":"$curve","children":"$children"}}],"params":{"value":null,"axis":null,"curve":null,"children":null}},"scaleTransition":{"variants":[{"template":{"kind":"musicxx.ScaleTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}},"rotationTransition":{"variants":[{"template":{"kind":"musicxx.RotationTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.RotationTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}},"slideTransition":{"variants":[{"template":{"kind":"musicxx.SlideTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SlideTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}}};

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

  // 标题行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
  // 参数：text(text, 必填)
  kit.title = function (params, env) { return expand("title", params || {}, env); };

  // 次要说明行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
  // 参数：text(text, 必填)
  kit.hint = function (params, env) { return expand("hint", params || {}, env); };

  // 正文行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
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

  // 分隔线（覆盖基础 kit：自带上下留白）
  // 参数：
  kit.divider = function (params, env) { return expand("divider", params || {}, env); };

  // 按钮（覆盖基础 kit：自带上下留白）
  // 参数：label(text, 必填)、variant(enum:buttonVariant, 默认 secondary)、icon(string)、disabled(bool, 默认 false)、action(action)
  kit.button = function (params, env) { return expand("button", params || {}, env); };

  // 一排等宽按钮（按钮列表里的每一项占一等份）
  // 参数：buttons(items, 必填)
  kit.actionsRow = function (params, env) { return expand("actionsRow", params || {}, env); };

  // 内容块（卡片，覆盖基础 kit：留白在这里声明，框架不再给隐含边距）
  // 参数：title(text)、variant(enum:blockVariant, 默认 card)、padding(edges, 默认 {horizontal: 50, vertical: 50})、margin(edges, 默认 {vertical: 50})、children(items)
  kit.card = function (params, env) { return expand("card", params || {}, env); };

  // 卡片里的一行（不带底色：卡片里直接按列表排列；右侧状态用说明字号）
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

  // 列表项（数据对象，给 ListViewBase / ListView 的 items 用）
  // 参数：id(string)、title(text, 必填)、subtitle(text)、trailing(text)、image(string)、icon(string)、tone(tone, 默认 normal)、selected(bool, 默认 false)、action(action)
  kit.listItem = function (params, env) { return expand("listItem", params || {}, env); };

  // 菜单项（数据对象，给 ListView 的 menu / itemMenu 用）
  // 参数：title(text, 必填)、icon(string)、action(action)
  kit.menuItem = function (params, env) { return expand("menuItem", params || {}, env); };

  // 迷你趋势图
  // 参数：data(numbers, 必填)、height(int, 默认 1)、glyphStyle(enum:sparkStyle, 默认 block)、showLast(bool, 默认 true)、tone(tone, 默认 accent)
  kit.sparkline = function (params, env) { return expand("sparkline", params || {}, env); };

  // 一行进度（覆盖基础 kit：musicxx 默认按百分比显示数值）
  // 参数：label(text)、value(float, 必填)、total(float, 默认 100)、unit(string, 默认 %)
  kit.progressRow = function (params, env) { return expand("progressRow", params || {}, env); };

  // 目录列表：一层目录的条目 + 顶部路径栏（栏里的「上一级」按钮）+ 可选的多选勾选框；进入目录与勾选态都由插件自己维护（不认识它的客户端退回普通列表；行样式由客户端定）
  // 参数：path(text)、items(listItems, 必填)、multi(bool, 默认 false)、showPath(bool, 默认 true)、pathAction(action)、itemAction(action)、selectAction(action)、height(size)、emptyText(text)
  kit.dirListView = function (params, env) { return expand("dirListView", params || {}, env); };

  // 按窗口宽高比切换两套内容：竖屏（宽高比 < 1）用 columnBody、横屏用 rowBody；给了第二套就按比例分两栅；不认识它的客户端用 rowBody
  // 参数：columnBody(items, 必填)、rowBody(items, 必填)、columnSecondBody(items)、rowSecondBody(items)、rowBodyRatio(float, 默认 0.5)、columnBodyRatio(float, 默认 0.5)、enableAnimations(bool, 默认 false)
  kit.adaptLayout = function (params, env) { return expand("adaptLayout", params || {}, env); };

  // 设置行：标题 + 说明 + 右侧当前值（整行可点且不带底色，右侧值用说明字号）
  // 参数：title(text, 必填)、depict(text)、value(text)、action(action)
  kit.settingRow = function (params, env) { return expand("settingRow", params || {}, env); };

  // 开关行：标题 + 说明 + 右侧开关（不带底色，值变化即派发动作、参数带 id 与当前值）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、action(action)
  kit.switchRow = function (params, env) { return expand("switchRow", params || {}, env); };

  // 输入行：标题 + 说明 + 输入框（不带底色，输入完成即派发动作）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、multiline(bool, 默认 false)、action(action)
  kit.inputRow = function (params, env) { return expand("inputRow", params || {}, env); };

  // 密码输入行：标题 + 说明 + 遮住的输入框（带眼睛按钮，输入完成即派发动作）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、action(action)
  kit.passwordRow = function (params, env) { return expand("passwordRow", params || {}, env); };

  // 下拉选择行：标题 + 说明 + 下拉框（选中即派发动作；候选项只有一行标题时用它）
  // 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、options(options, 必填)、action(action)
  kit.selectRow = function (params, env) { return expand("selectRow", params || {}, env); };

  // 点击打开浮层的行（零代码：动作就是官方动作 musicxx.ui.overlay.show）
  // 参数：label(text, 必填)、value(text)、title(text)、depict(text)、blocks(items, 必填)、textConfirm(text)
  kit.overlayRow = function (params, env) { return expand("overlayRow", params || {}, env); };

  // 插件着色器块（musicxx 专属组件；终端之类不认识它的客户端会跳过）
  // 参数：bundle(string, 必填)、args(json)、speed(float, 默认 1)、maxFps(int)、animate(bool, 默认 true)、resolutionScale(float, 默认 1)
  kit.shaderBlock = function (params, env) { return expand("shaderBlock", params || {}, env); };

  // 带封面的行：封面 + 标题 + 副标题 + 可选右侧状态（不带底色；取不到图片时显示标题）
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
