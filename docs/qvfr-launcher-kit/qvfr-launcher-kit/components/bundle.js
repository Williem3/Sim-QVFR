/* @ds-bundle: {"format":4,"namespace":"QVFR","components":[{"name":"Button"},{"name":"Checkbox"},{"name":"PopUpButton"},{"name":"SegmentedControl"},{"name":"PercentSlider"},{"name":"FormSection"},{"name":"FormRow"},{"name":"Sidebar"},{"name":"WindowChrome"},{"name":"StatusBadge"},{"name":"FoveationPreview"},{"name":"LauncherWindow"}]} */
(function () {
  var React = window.React;
  var h = React.createElement;
  var useState = React.useState, useEffect = React.useEffect, useRef = React.useRef, useId = React.useId;

  function cx() { return Array.prototype.filter.call(arguments, Boolean).join(' '); }
  function omit(o, keys) { var r = {}; for (var k in o) if (keys.indexOf(k) < 0) r[k] = o[k]; return r; }
  function clamp(n, a, b) { return Math.min(b, Math.max(a, n)); }
  function svg(props, kids) { return h('svg', Object.assign({ width: 16, height: 16, viewBox: '0 0 16 16', fill: 'none', stroke: 'currentColor', strokeWidth: 1.5, strokeLinecap: 'round', strokeLinejoin: 'round', 'aria-hidden': true }, props), kids); }

  var ICONS = {
    layers: [h('path', { key: 1, d: 'M8 2l6 3-6 3-6-3 6-3z' }), h('path', { key: 2, d: 'M2 8.5l6 3 6-3' }), h('path', { key: 3, d: 'M2 11.5l6 3 6-3' })],
    display: [h('rect', { key: 1, x: 1.75, y: 2.75, width: 12.5, height: 8.5, rx: 1.5 }), h('path', { key: 2, d: 'M5.5 14h5M8 11.25V14' })],
    eye: [h('path', { key: 1, d: 'M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z' }), h('circle', { key: 2, cx: 8, cy: 8, r: 2 })],
    target: [h('rect', { key: 1, x: 1.75, y: 1.75, width: 12.5, height: 12.5, rx: 2, strokeDasharray: '2 2' }), h('rect', { key: 2, x: 5, y: 5, width: 6, height: 6, rx: 1 })],
    info: [h('circle', { key: 1, cx: 8, cy: 8, r: 6.25 }), h('path', { key: 2, d: 'M8 7.25V11' }), h('circle', { key: 3, cx: 8, cy: 5, r: 0.6, fill: 'currentColor' })]
  };
  function Icon(p) { return svg({}, ICONS[p.name] || ICONS.info); }

  /* Button */
  function Button(p) {
    var v = p.variant || 'secondary', s = p.size || 'regular';
    var rest = omit(p, ['variant', 'size', 'className', 'children']);
    return h('button', Object.assign({ type: 'button' }, rest, { className: cx('qv-btn', 'qv-btn-' + v, s === 'large' && 'qv-btn-large', p.className) }), p.children);
  }

  /* Checkbox */
  function Checkbox(p) {
    var id = useId();
    var controlled = p.checked !== undefined;
    var st = useState(!!p.defaultChecked);
    var on = controlled ? p.checked : st[0];
    function change(e) { if (!controlled) st[1](e.target.checked); if (p.onChange) p.onChange(e.target.checked); }
    return h('div', { className: cx('qv-check', p.disabled && 'is-disabled', p.className) },
      h('input', { id: id, type: 'checkbox', className: 'qv-check-input', checked: on, disabled: p.disabled, onChange: change, 'aria-describedby': p.description ? id + 'd' : undefined }),
      h('div', { className: 'qv-check-text' },
        h('label', { htmlFor: id, className: 'qv-check-label' }, p.label),
        p.description ? h('div', { id: id + 'd', className: 'qv-check-desc' }, p.description) : null));
  }

  /* PopUpButton */
  function PopUpButton(p) {
    var sel = { id: p.id, className: 'qv-popup-select', disabled: p.disabled, 'aria-label': p['aria-label'], 'aria-labelledby': p['aria-labelledby'], style: p.width ? { minWidth: p.width } : undefined, onChange: function (e) { if (p.onChange) p.onChange(e.target.value); } };
    if (p.value !== undefined) sel.value = p.value; else sel.defaultValue = p.defaultValue;
    return h('span', { className: cx('qv-popup', p.className) },
      h('select', sel, p.options.map(function (o) { return h('option', { key: o.value, value: o.value, disabled: o.disabled }, o.label); })),
      h('svg', { className: 'qv-popup-chev', width: 8, height: 12, viewBox: '0 0 8 12', fill: 'none', stroke: 'currentColor', strokeWidth: 1.5, strokeLinecap: 'round', strokeLinejoin: 'round', 'aria-hidden': true }, h('path', { d: 'M1.5 4.25L4 1.75l2.5 2.5M1.5 7.75L4 10.25l2.5-2.5' })));
  }

  /* SegmentedControl */
  function SegmentedControl(p) {
    var ref = useRef(null);
    var controlled = p.value !== undefined;
    var st = useState(p.defaultValue !== undefined ? p.defaultValue : p.options[0].value);
    var val = controlled ? p.value : st[0];
    function pick(v, focus) {
      if (p.disabled) return;
      if (!controlled) st[1](v);
      if (p.onChange) p.onChange(v);
      if (focus && ref.current) setTimeout(function () { var b = ref.current.querySelector('[data-v="' + v + '"]'); if (b) b.focus(); }, 0);
    }
    function key(e) {
      var n = p.options.length, i = p.options.findIndex(function (o) { return o.value === val; });
      if (e.key === 'ArrowRight' || e.key === 'ArrowDown') { e.preventDefault(); pick(p.options[(i + 1) % n].value, true); }
      if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') { e.preventDefault(); pick(p.options[(i - 1 + n) % n].value, true); }
    }
    return h('div', { ref: ref, role: 'radiogroup', 'aria-label': p['aria-label'], 'aria-disabled': p.disabled || undefined, className: cx('qv-seg', p.disabled && 'is-disabled', p.className), onKeyDown: key },
      p.options.map(function (o) {
        var on = o.value === val;
        return h('button', { type: 'button', key: o.value, 'data-v': o.value, role: 'radio', 'aria-checked': on, tabIndex: on ? 0 : -1, disabled: p.disabled, className: cx('qv-seg-item', on && 'is-selected'), onClick: function () { pick(o.value); } },
          o.swatch ? h('span', { className: 'qv-swatch', style: { background: o.swatch }, 'aria-hidden': true }) : null, o.label);
      }));
  }

  /* PercentSlider */
  function PercentSlider(p) {
    var min = p.min != null ? p.min : 0, max = p.max != null ? p.max : 100, step = p.step || 1;
    var controlled = p.value !== undefined;
    var st = useState(p.defaultValue != null ? p.defaultValue : min);
    var val = controlled ? p.value : st[0];
    var id = useId();
    function change(e) { var n = Number(e.target.value); if (!controlled) st[1](n); if (p.onChange) p.onChange(n); }
    var fill = ((val - min) / (max - min)) * 100;
    var detail = typeof p.detail === 'function' ? p.detail(val) : p.detail;
    return h('div', { className: cx('qv-slider', p.disabled && 'is-disabled', p.className), style: p.width ? { width: p.width } : undefined },
      h('div', { className: 'qv-slider-row' },
        h('input', {
          id: id, type: 'range', className: 'qv-slider-input', min: min, max: max, step: step, value: val, disabled: p.disabled, onChange: change,
          'aria-label': p['aria-label'], 'aria-labelledby': p['aria-labelledby'],
          'aria-valuetext': val + ' percent' + (typeof detail === 'string' ? ', ' + detail : ''),
          style: { '--qv-fill': fill + '%' }
        }),
        h('output', { htmlFor: id, className: 'qv-slider-value' }, val + '%')),
      detail ? h('div', { className: 'qv-slider-detail', 'aria-hidden': true }, detail) : null);
  }

  /* FormSection + FormRow */
  function FormSection(p) {
    return h('section', { className: cx('qv-section', p.className) },
      p.title ? h('h3', { className: 'qv-section-title' }, p.title) : null,
      h('div', { className: 'qv-section-box' }, p.children),
      p.footer ? h('p', { className: 'qv-section-footer' }, p.footer) : null);
  }
  function FormRow(p) {
    if (!p.label) return h('div', { className: cx('qv-row', 'is-full', p.disabled && 'is-disabled') }, p.children);
    return h('div', { className: cx('qv-row', p.disabled && 'is-disabled') },
      h('div', { className: 'qv-row-text' },
        h('div', { className: 'qv-row-label', id: p.labelId }, p.label),
        p.description ? h('div', { className: 'qv-row-desc' }, p.description) : null),
      h('div', { className: 'qv-row-control' }, p.children));
  }

  /* Sidebar */
  var warnGlyph = h('svg', { width: 10, height: 10, viewBox: '0 0 10 10', 'aria-hidden': true }, h('path', { d: 'M5 2v3.5', stroke: 'currentColor', strokeWidth: 1.6, strokeLinecap: 'round' }), h('circle', { cx: 5, cy: 7.75, r: 0.9, fill: 'currentColor' }));
  function Sidebar(p) {
    return h('nav', { className: cx('qv-sidebar', p.className), 'aria-label': p['aria-label'] || 'Settings' },
      p.heading ? h('div', { className: 'qv-sidebar-heading' }, p.heading) : null,
      h('ul', { className: 'qv-sidebar-list' }, p.items.map(function (it) {
        var on = it.id === p.selected;
        return h('li', { key: it.id },
          h('button', { type: 'button', className: cx('qv-sidebar-item', on && 'is-selected'), 'aria-current': on ? 'page' : undefined, onClick: function () { if (p.onSelect) p.onSelect(it.id); } },
            h(Icon, { name: it.icon }), h('span', null, it.label),
            it.attention ? h('span', { className: 'qv-sidebar-badge', title: it.attention, 'aria-label': it.attention }, warnGlyph) : null));
      })));
  }

  /* WindowChrome */
  function WindowChrome(p) {
    var mac = (p.controls || 'mac') === 'mac';
    return h('div', { className: cx('qv-window', p.className), style: { width: p.width, height: p.height } },
      h('div', { className: 'qv-titlebar' },
        mac ? h('div', { className: 'qv-traffic' },
          h('span', { className: 'qv-tl qv-tl-close', role: 'img', 'aria-label': 'Close' }),
          h('span', { className: 'qv-tl qv-tl-min', role: 'img', 'aria-label': 'Minimize' }),
          h('span', { className: 'qv-tl qv-tl-zoom', role: 'img', 'aria-label': 'Zoom' })) : null,
        h('div', { className: 'qv-titlebar-title' }, p.title),
        p.toolbar ? h('div', { className: 'qv-titlebar-tools' }, p.toolbar) : null,
        !mac ? h('div', { className: 'qv-caption' },
          h('span', { role: 'img', 'aria-label': 'Minimize' }, svg({ width: 10, height: 10, viewBox: '0 0 10 10', strokeWidth: 1 }, h('path', { d: 'M0 5h10' }))),
          h('span', { role: 'img', 'aria-label': 'Maximize' }, svg({ width: 10, height: 10, viewBox: '0 0 10 10', strokeWidth: 1 }, h('rect', { x: 0.5, y: 0.5, width: 9, height: 9, rx: 1 }))),
          h('span', { role: 'img', 'aria-label': 'Close' }, svg({ width: 10, height: 10, viewBox: '0 0 10 10', strokeWidth: 1 }, h('path', { d: 'M0.5 0.5l9 9M9.5 0.5l-9 9' })))) : null),
      h('div', { className: 'qv-window-body' }, p.children));
  }

  /* StatusBadge */
  function StatusGlyph(p) {
    var t = p.tone;
    var kids = t === 'success' ? [h('circle', { key: 1, cx: 6, cy: 6, r: 5 }), h('path', { key: 2, d: 'M3.75 6.1l1.5 1.5 3-3.2' })]
      : t === 'warning' ? [h('path', { key: 1, d: 'M6 1.5l5 9H1z' }), h('path', { key: 2, d: 'M6 5v2.2' }), h('circle', { key: 3, cx: 6, cy: 8.8, r: 0.4, fill: 'currentColor' })]
      : t === 'danger' ? [h('circle', { key: 1, cx: 6, cy: 6, r: 5 }), h('path', { key: 2, d: 'M4.25 4.25l3.5 3.5M7.75 4.25l-3.5 3.5' })]
      : [h('circle', { key: 1, cx: 6, cy: 6, r: 2.5, fill: 'currentColor', stroke: 'none' })];
    return svg({ width: 12, height: 12, viewBox: '0 0 12 12', strokeWidth: 1.3 }, kids);
  }
  function StatusBadge(p) {
    var tone = p.tone || 'neutral';
    return h('span', { className: cx('qv-badge', 'qv-badge-' + tone, p.className), role: p.live ? 'status' : undefined }, h(StatusGlyph, { tone: tone }), h('span', null, p.children));
  }

  /* FoveationPreview */
  var OUTLINE = { teal: 'var(--overlay-teal)', magenta: 'var(--overlay-magenta)', green: 'var(--overlay-green)' };
  function FoveationPreview(p) {
    var outer = p.outer || { width: 1292, height: 1080 };
    var inner = p.inner || { width: 1008, height: 843 };
    var size = p.focusSize || { width: 45, height: 40 };
    var W = 320, H = Math.round(W * outer.height / outer.width);
    var fw = W * size.width / 100, fh = H * size.height / 100;
    var gaze = p.mode === 'eye' ? (p.gaze || { x: 0.6, y: 0.45 }) : { x: 0.5, y: 0.5 };
    var fx = clamp(gaze.x * W - fw / 2, 0, W - fw), fy = clamp(gaze.y * H - fh / 2, 0, H - fh);
    var ratio = (inner.width / fw) / (outer.width / W);
    var pc = 8, fc = clamp(pc / ratio, 2, 8);
    var uid = 'fv' + useId().replace(/[^a-zA-Z0-9]/g, '');
    var tfi = p.method === 'tfi';
    var band = 10;
    var focusFill = h('rect', { x: fx, y: fy, width: fw, height: fh, style: { fill: 'url(#' + uid + 'f)' } });
    return h('figure', { className: cx('qv-fov', p.className), style: { margin: 0 } },
      h('div', { className: 'qv-fov-frame' },
        h('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'Foveation preview: focus region ' + size.width + '% by ' + size.height + '% of the outer view, ' + (p.mode === 'eye' ? 'following gaze' : 'fixed at center') + '.' },
          h('defs', null,
            h('pattern', { id: uid + 'p', width: pc, height: pc, patternUnits: 'userSpaceOnUse' },
              h('rect', { width: pc, height: pc, style: { fill: 'var(--preview-periphery)' } }),
              h('path', { d: 'M' + pc + ' 0H0V' + pc, fill: 'none', style: { stroke: 'var(--preview-canvas)' }, strokeWidth: 0.6 })),
            h('pattern', { id: uid + 'f', width: fc, height: fc, patternUnits: 'userSpaceOnUse' },
              h('rect', { width: fc, height: fc, style: { fill: 'var(--preview-focus)' } }),
              h('path', { d: 'M' + fc + ' 0H0V' + fc, fill: 'none', style: { stroke: 'var(--preview-periphery)' }, strokeWidth: 0.4 })),
            h('pattern', { id: uid + 'i', width: 4, height: 4, patternUnits: 'userSpaceOnUse' },
              h('rect', { width: 4, height: 2, style: { fill: 'var(--preview-canvas)' }, opacity: 0.55 })),
            h('filter', { id: uid + 'b', x: '-20%', y: '-20%', width: '140%', height: '140%' }, h('feGaussianBlur', { stdDeviation: 4 }))),
          h('rect', { width: W, height: H, style: { fill: 'url(#' + uid + 'p)' } }),
          tfi ? h('rect', { width: W, height: H, style: { fill: 'url(#' + uid + 'i)' } }) : null,
          p.edgeBlend ? h('rect', { x: fx - band, y: fy - band, width: fw + band * 2, height: fh + band * 2, rx: 4, style: { fill: 'url(#' + uid + 'f)' }, opacity: 0.45 }) : null,
          p.feather ? h('g', { filter: 'url(#' + uid + 'b)' }, focusFill) : focusFill,
          p.feather ? h('rect', { x: fx + 6, y: fy + 6, width: Math.max(0, fw - 12), height: Math.max(0, fh - 12), style: { fill: 'url(#' + uid + 'f)' } }) : null,
          p.showOutline ? h('rect', { x: fx + 1, y: fy + 1, width: fw - 2, height: fh - 2, fill: 'none', style: { stroke: OUTLINE[p.outlineColor || 'teal'] }, strokeWidth: 2 }) : null,
          p.mode === 'eye' ? h('g', { style: { stroke: 'var(--preview-label)' }, strokeWidth: 1.2, fill: 'none' },
            h('circle', { cx: gaze.x * W, cy: gaze.y * H, r: 5 }),
            h('path', { d: 'M' + (gaze.x * W - 9) + ' ' + gaze.y * H + 'h4M' + (gaze.x * W + 5) + ' ' + gaze.y * H + 'h4M' + gaze.x * W + ' ' + (gaze.y * H - 9) + 'v4M' + gaze.x * W + ' ' + (gaze.y * H + 5) + 'v4' })) : null,
          h('text', { x: fx + 6, y: fy + 13, style: { fill: 'var(--preview-label)', fontFamily: 'var(--font-system)' }, fontSize: 9.5 }, 'Focus ' + inner.width + ' × ' + inner.height),
          h('text', { x: 8, y: H - 8, style: { fill: 'var(--preview-label)', fontFamily: 'var(--font-system)' }, fontSize: 9.5 }, 'Outer ' + outer.width + ' × ' + outer.height))),
      p.caption === false ? null : h('figcaption', { className: 'qv-fov-caption' },
        h('span', null, tfi ? 'Two-loop Foveated Interlaced' : 'Quad View Foveated Rendering'), h('span', { 'aria-hidden': true }, '·'),
        h('span', null, p.mode === 'eye' ? 'Eye tracked' : 'Fixed'), h('span', { 'aria-hidden': true }, '·'),
        h('span', null, 'Focus density ' + Math.round(ratio * 100) + '%')));
  }

  /* LauncherWindow */
  var DEFAULTS = { method: 'qvfr', mode: 'eye', outer: 50, inner: 100, size: { width: 39, height: 39 }, outline: false, outlineColor: 'teal', blend: true, feather: false };
  var RANGES = { outer: { min: 20, max: 100 }, inner: { min: 50, max: 200 }, size: { min: 10, max: 100 } };
  function viewPixels(st, base) {
    return {
      outer: { width: Math.round(base.width * st.outer / 100), height: Math.round(base.height * st.outer / 100) },
      inner: { width: Math.round(base.width * st.inner / 100 * st.size.width / 100), height: Math.round(base.height * st.inner / 100 * st.size.height / 100) }
    };
  }
  function px(s) { return s.width + ' × ' + s.height + ' px'; }
  var PANES = [
    { id: 'rendering', label: 'Rendering', icon: 'layers' },
    { id: 'resolution', label: 'Resolution', icon: 'display' },
    { id: 'foveation', label: 'Foveation', icon: 'eye' },
    { id: 'overlay', label: 'Focus overlay', icon: 'target' },
    { id: 'about', label: 'About', icon: 'info' }
  ];
  function LauncherWindow(p) {
    var init = Object.assign({}, DEFAULTS, p.defaultSettings || {});
    var s = useState(init), st = s[0];
    var pane = useState(p.defaultPane || 'rendering');
    var launch = useState('idle');
    var eyeOK = p.eyeTrackingAvailable !== false;
    function set(k, v) { var n = Object.assign({}, st); n[k] = v; s[1](n); launch[1]('idle'); if (p.onChange) p.onChange(n); }
    var base = p.recommendedResolution || { width: 2584, height: 2160 };
    var vp = viewPixels(st, base);
    var pix = 2 * (vp.outer.width * vp.outer.height + vp.inner.width * vp.inner.height), basePix = 2 * base.width * base.height;
    var mpix = (pix / 1e6).toFixed(2) + ' M of ' + (basePix / 1e6).toFixed(2) + ' M';
    var savedPct = Math.round(100 * (1 - pix / basePix));
    var saved = savedPct >= 0 ? savedPct + '%' : 'None (' + -savedPct + '% more)';
    function setSize(axis, v) { var n = Object.assign({}, st.size); n[axis] = v; set('size', n); }
    var eyeWarn = st.mode === 'eye' && !eyeOK;
    var items = PANES.map(function (x) { return x.id === 'foveation' && eyeWarn ? Object.assign({}, x, { attention: 'Eye tracker not detected' }) : x; });
    var content;
    if (pane[0] === 'rendering') content = [
      h(FormSection, { key: 'a', title: 'Rendering', footer: 'Quad View Foveated Rendering draws a sharp focus view inside a lower-resolution outer view for each eye.' },
        h(FormRow, { label: 'Rendering method', labelId: 'lbl-method' }, h(PopUpButton, { 'aria-labelledby': 'lbl-method', value: st.method, onChange: function (v) { set('method', v); }, options: [{ value: 'qvfr', label: 'Quad View Foveated Rendering' }, { value: 'tfi', label: 'Two-loop Foveated Interlaced' }] })),
        h(FormRow, { label: 'Foveated mode', labelId: 'lbl-mode', description: st.mode === 'eye' ? 'The focus region follows your gaze.' : 'The focus region stays at the center of each lens.' },
          h(PopUpButton, { 'aria-labelledby': 'lbl-mode', value: st.mode, onChange: function (v) { set('mode', v); }, width: 140, options: [{ value: 'eye', label: 'Eye tracked' }, { value: 'fixed', label: 'Fixed' }] })))];
    else if (pane[0] === 'resolution') content = [
      h(FormSection, { key: 'a', title: 'Per-eye resolution', footer: 'Percent of the headset’s recommended resolution (' + base.width + ' × ' + base.height + ' px per eye). Outer and inner are set independently. Applies at next launch.' },
        h(FormRow, { label: 'Outer resolution', labelId: 'lbl-outer', description: 'The full field of view, rendered at lower density.' },
          h(PercentSlider, { 'aria-labelledby': 'lbl-outer', min: RANGES.outer.min, max: RANGES.outer.max, value: st.outer, onChange: function (v) { set('outer', v); }, detail: px(vp.outer) })),
        h(FormRow, { label: 'Inner resolution', labelId: 'lbl-inner', description: 'Pixel density inside the focus region.' },
          h(PercentSlider, { 'aria-labelledby': 'lbl-inner', min: RANGES.inner.min, max: RANGES.inner.max, value: st.inner, onChange: function (v) { set('inner', v); }, detail: px(vp.inner) })))];
    else if (pane[0] === 'foveation') content = [
      eyeWarn ? h(StatusBadge, { key: 'w', tone: 'warning', live: true }, 'Eye tracker not detected. The focus region stays centered until one connects.') : null,
      h(FormSection, { key: 'a', title: 'Focus region', footer: 'Share of the outer view that the focus view covers.' },
        h(FormRow, { label: 'Foveated width', labelId: 'lbl-fw' },
          h(PercentSlider, { 'aria-labelledby': 'lbl-fw', min: RANGES.size.min, max: RANGES.size.max, value: st.size.width, onChange: function (v) { setSize('width', v); } })),
        h(FormRow, { label: 'Foveated height', labelId: 'lbl-fh' },
          h(PercentSlider, { 'aria-labelledby': 'lbl-fh', min: RANGES.size.min, max: RANGES.size.max, value: st.size.height, onChange: function (v) { setSize('height', v); } })))];
    else if (pane[0] === 'overlay') content = [
      h(FormSection, { key: 'a', title: 'Focus overlay', footer: 'Overlays are drawn in the headset only. They never appear in replays or the mirror window.' },
        h(FormRow, null, h(Checkbox, { label: 'Show focus outline', description: 'Draws a colored border around the focus region.', checked: st.outline, onChange: function (v) { set('outline', v); } })),
        h(FormRow, { label: 'Outline color', disabled: !st.outline },
          h(SegmentedControl, { 'aria-label': 'Outline color', disabled: !st.outline, value: st.outlineColor, onChange: function (v) { set('outlineColor', v); }, options: [{ value: 'teal', label: 'Teal', swatch: OUTLINE.teal }, { value: 'magenta', label: 'Magenta', swatch: OUTLINE.magenta }, { value: 'green', label: 'Green', swatch: OUTLINE.green }] })),
        h(FormRow, null, h(Checkbox, { label: 'Focus edge blend', description: 'Cross-fades the focus and outer views across a narrow band.', checked: st.blend, onChange: function (v) { set('blend', v); } })),
        h(FormRow, null, h(Checkbox, { label: 'Feather', description: 'Softens the edge of the focus region.', checked: st.feather, onChange: function (v) { set('feather', v); } })))];
    else content = [h('div', { key: 'a', className: 'qv-about' }, h('strong', null, 'QVFR Launcher'), h('span', null, 'Starts Assetto Corsa EVO with foveated rendering enabled.'), h('span', null, 'Version ' + (p.version || '1.0.0')))];
    var title = PANES.find(function (x) { return x.id === pane[0]; }).label;
    var badge = launch[0] === 'launching' ? h(StatusBadge, { tone: 'neutral', live: true }, 'Starting Assetto Corsa EVO…')
      : eyeWarn ? h(StatusBadge, { tone: 'warning', live: true }, 'Eye tracker not detected')
      : h(StatusBadge, { tone: 'success', live: true }, 'Ready');
    return h(WindowChrome, { title: 'QVFR Launcher', width: p.width || '100%', height: p.height || 600, controls: p.controls },
      h('div', { className: 'qv-launcher' },
        h(Sidebar, { items: items, selected: pane[0], onSelect: pane[1] }),
        h('div', { className: 'qv-launcher-main' }, h('h2', { className: 'qv-launcher-title' }, title), content),
        h('aside', { className: 'qv-launcher-aside', 'aria-label': 'Preview' },
          h('h3', null, 'Preview'),
          h(FoveationPreview, { method: st.method, mode: eyeWarn ? 'fixed' : st.mode, outer: vp.outer, inner: vp.inner, focusSize: st.size, showOutline: st.outline, outlineColor: st.outlineColor, edgeBlend: st.blend, feather: st.feather }),
          h('dl', { className: 'qv-readouts' },
            h('dt', null, 'Runtime resolution'), h('dd', null, base.width + ' × ' + base.height),
            h('dt', null, 'Outer view'), h('dd', null, vp.outer.width + ' × ' + vp.outer.height),
            h('dt', null, 'Focus view'), h('dd', null, vp.inner.width + ' × ' + vp.inner.height),
            h('dt', null, 'Pixels per frame'), h('dd', null, mpix),
            h('dt', null, 'Pixels saved'), h('dd', null, saved)))),
      h('div', { className: 'qv-actionbar' },
        badge,
        h('span', { className: 'qv-actionbar-spacer' }),
        h(Button, { onClick: function () { s[1](Object.assign({}, DEFAULTS)); launch[1]('idle'); } }, 'Restore Defaults'),
        h(Button, { variant: 'primary', size: 'large', onClick: function () { launch[1]('launching'); if (p.onLaunch) p.onLaunch(st); } }, 'Launch Assetto Corsa EVO')));
  }

  var api = { Button: Button, Checkbox: Checkbox, PopUpButton: PopUpButton, SegmentedControl: SegmentedControl, PercentSlider: PercentSlider, FormSection: FormSection, FormRow: FormRow, Sidebar: Sidebar, WindowChrome: WindowChrome, StatusBadge: StatusBadge, FoveationPreview: FoveationPreview, LauncherWindow: LauncherWindow, defaults: DEFAULTS, ranges: RANGES, viewPixels: viewPixels };
  window.QVFR = Object.assign(window.QVFR || {}, api);
})();
