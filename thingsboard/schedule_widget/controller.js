/*
 * OpenTherm boiler: weekly schedule widget for ThingsBoard ("Latest values").
 *
 * Datasource: the device; data keys (attributes):
 *   schedule - client attribute from the board, {"mon":[["06:00",55],...x4],...}
 *   mode     - optional, shows a warning when the schedule is not in effect
 * Saving writes the shared attribute set_schedule (same JSON as a string);
 * the board applies it at once, or on reconnect if it is offline, and reports
 * the result back in `schedule`.
 */

var SCHED_KEY = 'schedule';
var MODE_KEY = 'mode';
var SET_KEY = 'set_schedule';
var DAYS = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
var DAY_NAMES = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс'];
var POINTS = 4;
var T_MAX = 80; /* firmware limit, degC */
var T_OFF = 20; /* below: boiler off */
var CONFIRM_MS = 20000;

var st;

self.onInit = function () {
    st = {
        root: self.ctx.$container[0],
        sched: null,   /* [day][{m, t}] from the board, t in 0.1 degC */
        received: false,
        mode: null,
        edit: null,    /* [day][{time, temp}] strings while editing */
        pending: null, /* what was saved and not yet confirmed */
        msg: '',
        msgKind: ''
    };
    st.root.innerHTML =
        '<div class="ots">' +
        '<div class="ots-head"><div class="ots-note"></div>' +
        '<div class="ots-buttons">' +
        '<button class="ots-btn ots-edit">Изменить</button>' +
        '<button class="ots-btn ots-cancel">Отмена</button>' +
        '<button class="ots-btn ots-primary ots-save">Сохранить</button>' +
        '</div></div>' +
        '<div class="ots-warn"></div>' +
        '<div class="ots-body"></div>' +
        '<div class="ots-msg"></div>' +
        '</div>';
    st.el = {
        note: q('.ots-note'), warn: q('.ots-warn'), body: q('.ots-body'), msg: q('.ots-msg'),
        edit: q('.ots-edit'), cancel: q('.ots-cancel'), save: q('.ots-save')
    };
    st.el.note.textContent = 'Уставка подачи котла, °C. Ниже ' + T_OFF + ' °C котёл выключен. ' +
        'Действует в режиме auto.';
    st.el.edit.addEventListener('click', startEdit);
    st.el.cancel.addEventListener('click', function () {
        st.edit = null;
        render();
    });
    st.el.save.addEventListener('click', save);
    st.timer = setInterval(function () {
        if (!st.edit) {
            render();
        }
    }, 60000);
    self.onDataUpdated();
};

self.onDataUpdated = function () {
    if (!st) {
        return;
    }
    (self.ctx.data || []).forEach(function (d) {
        var v = d.data && d.data.length ? d.data[0][1] : undefined;
        if (d.dataKey.name === SCHED_KEY) {
            var s = parseSched(v);
            if (s) {
                st.sched = s;
            }
            st.received = st.received || v !== undefined;
        } else if (d.dataKey.name === MODE_KEY) {
            st.mode = v === undefined || v === '' ? null : String(v);
        }
    });
    if (st.pending && st.sched && sameSched(st.pending, st.sched)) {
        st.pending = null;
        setMsg('Плата применила расписание.', 'ok');
    }
    render();
};

self.onResize = function () {};

self.onDestroy = function () {
    if (st) {
        clearInterval(st.timer);
        clearTimeout(st.confirmTimer);
    }
};

self.typeParameters = function () {
    return { maxDatasources: 1, maxDataKeys: 2, dataKeysOptional: false, singleEntity: true };
};

/* ------------------------------------------------------------- helpers */

function q(sel) {
    return st.root.querySelector(sel);
}

function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
        return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
}

function hhmm(m) {
    return ('0' + Math.floor(m / 60)).slice(-2) + ':' + ('0' + (m % 60)).slice(-2);
}

function fmtT(x10) {
    return String(x10 / 10);
}

function parseTime(s) {
    var r = /^(\d{1,2}):(\d{2})$/.exec(String(s).trim());
    if (!r || +r[1] > 23 || +r[2] > 59) {
        return -1;
    }
    return +r[1] * 60 + +r[2];
}

function parseTemp(s) {
    s = String(s).trim().replace(',', '.');
    if (!/^-?\d+(\.\d+)?$/.test(s)) {
        return NaN;
    }
    var t = Math.round(parseFloat(s) * 10);
    return t < 0 || t > T_MAX * 10 ? NaN : t;
}

/* {"mon":[["06:00",55],...],...} (object or JSON text) -> [day][{m, t}] */
function parseSched(v) {
    var o = v;
    if (typeof v === 'string') {
        try {
            o = JSON.parse(v);
        } catch (e) {
            return null;
        }
    }
    if (!o || typeof o !== 'object') {
        return null;
    }
    var out = [];
    for (var d = 0; d < DAYS.length; d++) {
        var day = o[DAYS[d]];
        if (!Array.isArray(day) || !day.length) {
            return null;
        }
        var pts = [];
        for (var i = 0; i < day.length; i++) {
            var p = day[i];
            var m = Array.isArray(p) ? parseTime(p[0]) : -1;
            var t = Array.isArray(p) ? Math.round(Number(p[1]) * 10) : NaN;
            if (m < 0 || isNaN(t)) {
                return null;
            }
            pts.push({ m: m, t: t });
        }
        out.push(pts);
    }
    return out;
}

/* Distinct points of a day by time; as on the board, the first point listed
 * for a given minute wins. */
function dayPoints(pts) {
    var seen = {};
    var out = [];
    pts.forEach(function (p) {
        if (!seen[p.m]) {
            seen[p.m] = true;
            out.push(p);
        }
    });
    return out.sort(function (a, b) {
        return a.m - b.m;
    });
}

function sameSched(a, b) {
    for (var d = 0; d < DAYS.length; d++) {
        var x = dayPoints(a[d]);
        var y = dayPoints(b[d]);
        if (x.length !== y.length) {
            return false;
        }
        for (var i = 0; i < x.length; i++) {
            if (x[i].m !== y[i].m || x[i].t !== y[i].t) {
                return false;
            }
        }
    }
    return true;
}

function color(x10) {
    if (x10 < T_OFF * 10) {
        return 'rgba(128,128,128,0.25)';
    }
    var k = Math.min(1, (x10 / 10 - T_OFF) / (T_MAX - T_OFF));
    return 'hsl(' + Math.round(210 - 210 * k) + ',75%,55%)';
}

/* Monday = 0 */
function today() {
    return (new Date().getDay() + 6) % 7;
}

function setMsg(text, kind) {
    st.msg = text;
    st.msgKind = kind || '';
}

/* ------------------------------------------------------------- view */

function render() {
    var editing = !!st.edit;
    st.el.edit.style.display = editing ? 'none' : '';
    st.el.cancel.style.display = editing ? '' : 'none';
    st.el.save.style.display = editing ? '' : 'none';
    st.el.edit.disabled = !st.sched;

    var warn = '';
    if (st.mode && st.mode !== 'auto') {
        warn = 'Сейчас режим «' + st.mode + '»: расписание не действует, пока не включён режим auto.';
    }
    st.el.warn.textContent = warn;
    st.el.warn.style.display = warn ? '' : 'none';

    if (editing) {
        /* data updates must not redraw the inputs under the cursor */
        if (st.drawn !== 'edit') {
            renderEdit();
        }
        return;
    }
    st.el.msg.textContent = st.msg;
    st.el.msg.className = 'ots-msg' + (st.msgKind ? ' ots-' + st.msgKind : '');
    if (st.sched) {
        renderView();
    } else {
        st.drawn = 'empty';
        st.el.body.innerHTML = '<div class="ots-empty">' +
            (st.received ? 'Атрибут schedule не похож на расписание платы.'
                : 'Нет данных: выберите устройство и ключ-атрибут schedule.') + '</div>';
    }
}

function renderView() {
    var now = new Date();
    var nowPct = (now.getHours() * 60 + now.getMinutes()) / 14.4;
    var html = '<div class="ots-scale"><span></span><div>' +
        [0, 3, 6, 9, 12, 15, 18, 21].map(function (h) {
            return '<i style="left:' + (h / 0.24) + '%">' + h + '</i>';
        }).join('') + '</div><span></span></div>';

    for (var d = 0; d < DAYS.length; d++) {
        var pts = dayPoints(st.sched[d]);
        /* before the first point the last point of an earlier day holds */
        var carry = null;
        for (var b = 1; b <= DAYS.length && carry === null; b++) {
            var prev = dayPoints(st.sched[(d + DAYS.length - b) % DAYS.length]);
            if (prev.length) {
                carry = prev[prev.length - 1].t;
            }
        }
        var segs = [];
        var from = 0;
        var t = carry;
        pts.forEach(function (p) {
            if (p.m > from) {
                segs.push({ a: from, b: p.m, t: t });
            }
            from = p.m;
            t = p.t;
        });
        segs.push({ a: from, b: 1440, t: t });

        var bar = segs.map(function (s) {
            var w = (s.b - s.a) / 14.4;
            var off = s.t < T_OFF * 10;
            var label = w >= 7 ? (off ? 'выкл' : fmtT(s.t) + '°') : '';
            return '<div class="ots-seg' + (off ? ' ots-off' : '') + '" style="left:' + (s.a / 14.4) +
                '%;width:' + w + '%;background:' + color(s.t) + '" title="' + hhmm(s.a) + '–' +
                hhmm(s.b === 1440 ? 0 : s.b) + ': ' + fmtT(s.t) + ' °C' + (off ? ' (котёл выключен)' : '') +
                '">' + label + '</div>';
        }).join('');
        if (d === today()) {
            bar += '<div class="ots-now" style="left:' + nowPct + '%"></div>';
        }
        var text = pts.map(function (p) {
            return hhmm(p.m) + ' <b>' + fmtT(p.t) + '</b>';
        }).join('&nbsp;· ');

        html += '<div class="ots-row' + (d === today() ? ' ots-today' : '') + '">' +
            '<div class="ots-day">' + DAY_NAMES[d] + '</div>' +
            '<div class="ots-bar">' + bar + '</div>' +
            '<div class="ots-pts">' + text + '</div></div>';
    }
    st.drawn = 'view';
    st.el.body.innerHTML = html;
}

/* ------------------------------------------------------------- editor */

function startEdit() {
    if (!st.sched) {
        return;
    }
    st.edit = st.sched.map(function (day) {
        var pts = dayPoints(day);
        var out = [];
        for (var i = 0; i < POINTS; i++) {
            out.push(i < pts.length ? { time: hhmm(pts[i].m), temp: fmtT(pts[i].t) } : { time: '', temp: '' });
        }
        return out;
    });
    setMsg('', '');
    render();
}

function renderEdit() {
    var html = '<div class="ots-ehint">До 4 точек на день: с указанного времени действует уставка до ' +
        'следующей точки (через полночь тоже). Лишние точки оставьте пустыми.</div>';
    for (var d = 0; d < DAYS.length; d++) {
        html += '<div class="ots-erow' + (d === today() ? ' ots-today' : '') + '"><div class="ots-day">' +
            DAY_NAMES[d] + '</div><div class="ots-epts">';
        for (var i = 0; i < POINTS; i++) {
            var p = st.edit[d][i];
            html += '<span class="ots-pt">' +
                '<input type="time" step="60" data-d="' + d + '" data-i="' + i + '" data-f="time" value="' +
                esc(p.time) + '">' +
                '<input type="number" step="0.5" min="0" max="' + T_MAX + '" data-d="' + d + '" data-i="' + i +
                '" data-f="temp" value="' + esc(p.temp) + '" placeholder="°C">' +
                '</span>';
        }
        html += '</div><select class="ots-copy" data-d="' + d + '">' +
            '<option value="">Копировать…</option>' +
            '<option value="wd">в будни</option>' +
            '<option value="we">в выходные</option>' +
            '<option value="all">во все дни</option>' +
            '</select></div>';
    }
    st.drawn = 'edit';
    st.el.body.innerHTML = html;

    Array.prototype.forEach.call(st.el.body.querySelectorAll('input'), function (inp) {
        inp.addEventListener('input', function () {
            st.edit[+inp.dataset.d][+inp.dataset.i][inp.dataset.f] = inp.value;
            validate();
        });
    });
    Array.prototype.forEach.call(st.el.body.querySelectorAll('.ots-copy'), function (sel) {
        sel.addEventListener('change', function () {
            var src = +sel.dataset.d;
            var to = { wd: [0, 1, 2, 3, 4], we: [5, 6], all: [0, 1, 2, 3, 4, 5, 6] }[sel.value] || [];
            to.forEach(function (d) {
                if (d !== src) {
                    st.edit[d] = st.edit[src].map(function (p) {
                        return { time: p.time, temp: p.temp };
                    });
                }
            });
            renderEdit();
        });
    });
    validate();
}

/* Marks bad fields; returns [day][{m, t}] or null. */
function validate() {
    var out = [];
    var errors = [];
    for (var d = 0; d < DAYS.length; d++) {
        var used = [];
        var times = {};
        for (var i = 0; i < POINTS; i++) {
            var p = st.edit[d][i];
            var hasT = p.time.trim() !== '';
            var hasV = String(p.temp).trim() !== '';
            var m = hasT ? parseTime(p.time) : -1;
            var t = hasV ? parseTemp(p.temp) : NaN;
            var badT = (hasT || hasV) && m < 0;
            var badV = (hasT || hasV) && isNaN(t);
            var dup = !badT && hasT && times[m];
            if (dup) {
                errors.push(DAY_NAMES[d] + ': два раза ' + hhmm(m));
            } else if (badV && hasV) {
                errors.push(DAY_NAMES[d] + ': температура 0…' + T_MAX);
            } else if (badT || badV) {
                errors.push(DAY_NAMES[d] + ': точка заполнена не полностью');
            } else if (hasT) {
                times[m] = true;
                used.push({ m: m, t: t });
            }
            mark(d, i, 'time', badT || dup);
            mark(d, i, 'temp', badV);
        }
        if (!used.length) {
            errors.push(DAY_NAMES[d] + ': нужна хотя бы одна точка');
        }
        out.push(used);
    }
    var ok = !errors.length && !!(self.ctx.datasources && self.ctx.datasources[0]);
    st.el.save.disabled = !ok;
    st.el.msg.textContent = errors.length ? errors.filter(function (e, k) {
        return errors.indexOf(e) === k;
    }).join('; ') : '';
    st.el.msg.className = 'ots-msg' + (errors.length ? ' ots-err' : '');
    return ok ? out : null;
}

function mark(d, i, f, bad) {
    var inp = st.el.body.querySelector('input[data-d="' + d + '"][data-i="' + i + '"][data-f="' + f + '"]');
    if (inp) {
        inp.classList.toggle('ots-bad', bad);
    }
}

function save() {
    var days = validate();
    if (!days) {
        return;
    }
    /* the board takes exactly 4 points a day: unused ones repeat the last */
    var obj = {};
    var sched = [];
    days.forEach(function (used, d) {
        used.sort(function (a, b) {
            return a.m - b.m;
        });
        var full = used.slice();
        while (full.length < POINTS) {
            full.push(used[used.length - 1]);
        }
        obj[DAYS[d]] = full.map(function (p) {
            return [hhmm(p.m), p.t / 10];
        });
        sched.push(full);
    });

    var ds = self.ctx.datasources[0];
    var entityId = { entityType: ds.entityType, id: ds.entityId };
    st.el.save.disabled = true;
    self.ctx.attributeService.saveEntityAttributes(entityId, 'SHARED_SCOPE',
        [{ key: SET_KEY, value: JSON.stringify(obj) }]).subscribe(function () {
        st.edit = null;
        if (st.sched && sameSched(sched, st.sched)) {
            setMsg('Сохранено (расписание не изменилось).', 'ok');
        } else {
            st.pending = sched;
            setMsg('Сохранено, ждём подтверждения платы…', '');
            clearTimeout(st.confirmTimer);
            st.confirmTimer = setTimeout(function () {
                if (st.pending) {
                    setMsg('Сервер сохранил, плата пока не подтвердила: скорее всего, связь с ней ' +
                        'оборвалась. Расписание применится при переподключении, это сообщение ' +
                        'сменится само.', 'warn');
                    render();
                }
            }, CONFIRM_MS);
        }
        render();
    }, function (err) {
        /* still editing: nothing is lost, the user can retry */
        st.el.save.disabled = false;
        st.el.msg.textContent = 'Не удалось сохранить: ' +
            ((err && (err.message || err.statusText)) || 'ошибка сервера');
        st.el.msg.className = 'ots-msg ots-err';
    });
}
