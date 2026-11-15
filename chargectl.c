// chargectl v7 — Chromebook 充電キープ・ミニアプリの本体
// 役割: (1) 選んだ保持%を覚える (2) ectool で Battery Sustainer に適用する
//       (3) 再起動などで既定(80%)に戻されていたら入れ直す (4) 操作画面を出す
// 画面は http://127.0.0.1:8765/ （この機械の中だけ）。外部には開かない。
// v2 の変更点 (2026-11-13):
//   - バグ修正: ectool の実パスは /usr/sbin/ectool。v1 は /usr/bin/ectool と
//     書いていたため読み書きが全て無言で失敗していた（stderr を捨てていた）。
//   - 計装: ectool の実行結果（stdout+stderr と終了コード）を必ずログに残す。
//     ログは upstart 経由で /var/log/chargectl.log に出る（stderr へ書く）。
//   - 適用後は読み戻して照合し、外れたら1回だけ再試行する。
//   - 読み取り失敗（出力が空・終了コード非0・解釈不能）と「off」を区別する。
//     失敗時は keeper は何もせず、/api/state は known:false を返す。
// v3 の変更点 (2026-11-13):
//   - keeper の見回り周期を画面から選べるようにした（30秒/1分/5分/10分、
//     既定は30秒）。周期は interval ファイルに保存し、再起動後も維持する。
//   - 見回りの数え方を「前回の見回りからの経過時間」に変えた。旧方式は
//     select のタイムアウト依存で、画面を開いている間（5秒ごとの状態取得）
//     は実質止まっていた。周期を短く変えた場合は次周期を待たず即見回る。
// v4 の変更点 (2026-11-13):
//   - 充電速度（電池への充電電流の上限 mA）の段を追加: 500/1000/2000/3000/無制限。
//     speed ファイルに保存（0=無制限、ファイル無しも無制限＝純正の既定）。
//   - 実測で確定した制約: ectool chargecurrentlimit は書き込み専用で読み戻しが
//     無い。そのため速度は読み戻し照合ができない。keeper は見回りのたびに
//     保存値を盲目的に書き直す（冪等な書き込みなので無害）。ログは値が
//     変わったときと失敗時だけ出す。
//   - 実効は実測済み: 上限500を書くと充電電流 1269mA→491mA に落ち、
//     無制限（4294967295、powerd と同じ値）を書くと約1300mA に戻った。
// v5 の変更点 (2026-11-16):
//   - 画面の状態表示に電気量を追加: 電圧(V)・電流(mA)・電力(W)・残量(mAh)・
//     電池の状態（充電中/放電中など）。電池の状態ファイル
//     （/sys/class/power_supply/BAT*/ の voltage_now / current_now /
//     charge_now / status）を読むだけの表示専用で、制御には一切使わない。
//   - 電力は power_now が無い機種向けに電圧×電流で計算する。
// v6 の変更点 (2026-11-16):
//   - 電流の符号を状態で正規化する: 充電中は＋、放電中は−で表示する。
//     この機種の current_now は放電中でも正の大きさで返る（実測: 放電中
//     +614mA）ため、生値の符号は信用せず status を見て付け替える。
//     電力は大きさの話なので絶対値で計算する（画面側）。
// v7 の変更点 (2026-11-16):
//   - 電流と電池状態の文字に色を付ける: 充電中は緑、放電中はオレンジ、
//     それ以外は通常色。Ampere のような電池アプリの流儀に合わせた。
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT 8765
#define STATE_DIR "/usr/local/share/chargectl"
#define TARGET_FILE STATE_DIR "/target"
#define INTERVAL_FILE STATE_DIR "/interval"
#define SPEED_FILE STATE_DIR "/speed"
#define ECTOOL "/usr/sbin/ectool"
static const int CHOICES[] = {50, 55, 60, 70, 80, 100};
#define NCHOICES 6
static const int INT_CHOICES[] = {30, 60, 300, 600};
#define NINT 4
#define DEFAULT_INTERVAL 30
static const int SPD_CHOICES[] = {500, 1000, 2000, 3000, 0};
#define NSPD 5
#define UNLIMITED_MA 4294967295LL

// ---------- ログ ----------
static void log_msg(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // 改行や制御文字はログを壊さないよう空白に置く
    for (char *p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    fprintf(stderr, "chargectl[%ld]: %s\n", (long)time(NULL), msg);
    fflush(stderr);
}

// ---------- 小物 ----------
static int read_file_int(const char *path, int *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v;
    int ok = fscanf(f, "%d", &v);
    fclose(f);
    if (ok != 1) return -1;
    *out = v;
    return 0;
}

// コマンドを実行し stdout+stderr を buf に取る。戻り値は終了コード（-1=起動失敗）。
static int run_capture(const char *cmd, char *buf, size_t cap) {
    buf[0] = '\0';
    char full[512];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    if (!p) { log_msg("popen failed cmd=%s errno=%d", cmd, errno); return -1; }
    size_t n = fread(buf, 1, cap - 1, p);
    buf[n] = '\0';
    int st = pclose(p);
    if (st == -1) { log_msg("pclose failed cmd=%s errno=%d", cmd, errno); return -1; }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    log_msg("cmd did not exit normally cmd=%s status=%d raw=%s", cmd, st, buf);
    return -2;
}

// 現在の sustainer 状態を読む。戻り値 0=確定（on/off どちらか）、-1=不明（読み取り失敗）。
static int read_sustainer(bool *on, int *lower, int *upper) {
    char buf[1024];
    char cmd[128];
    snprintf(cmd, sizeof(cmd), ECTOOL " chargecontrol");
    int rc = run_capture(cmd, buf, sizeof(buf));
    *on = false; *lower = -1; *upper = -1;
    if (rc != 0 || buf[0] == '\0') {
        log_msg("read_sustainer FAILED rc=%d out=[%s]", rc, buf);
        return -1;
    }
    char *pos = strstr(buf, "Battery sustainer = on");
    if (pos) {
        int lo, hi;
        if (sscanf(pos, "Battery sustainer = on (%d%% ~ %d%%)", &lo, &hi) == 2) {
            *on = true; *lower = lo; *upper = hi;
            return 0;
        }
        log_msg("read_sustainer parse failed (on but no range) out=[%s]", buf);
        return -1;
    }
    if (strstr(buf, "Battery sustainer = off")) {
        *on = false;
        return 0;
    }
    log_msg("read_sustainer unknown output rc=%d out=[%s]", rc, buf);
    return -1;
}

static int read_battery_percent(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
            int v;
            if (read_file_int(path, &v) == 0 && v >= 0 && v <= 100) {
                closedir(d);
                return v;
            }
        }
        closedir(d);
    }
    return -1;
}

// 文字列ファイルを読む（末尾の改行は落とす）。電池の status 用。
static int read_file_str(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

// 電池の電気量を読む（表示専用）。電圧 mV・電流 mA・残量 mAh・状態文字列。
// 読めない項目は -1（状態は空文字）。電池ディレクトリは voltage_now を
// 持つものを探し、名前が BAT で始まるものを優先する（マウス等の電池と
// 取り違えないため）。
static void read_battery_elec(int *mv, int *ma, int *mah, char *status, size_t scap) {
    *mv = -1; *ma = -1; *mah = -1;
    if (scap > 0) status[0] = '\0';
    char dir[256] = "";
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    char fallback[256] = "";
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/voltage_now", e->d_name);
        if (access(path, R_OK) != 0) continue;
        if (strncmp(e->d_name, "BAT", 3) == 0) {
            snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", e->d_name);
            break;
        }
        if (fallback[0] == '\0')
            snprintf(fallback, sizeof(fallback), "/sys/class/power_supply/%s", e->d_name);
    }
    closedir(d);
    if (dir[0] == '\0') snprintf(dir, sizeof(dir), "%s", fallback);
    if (dir[0] == '\0') return;
    char path[512];
    int uv, ua, uah;
    snprintf(path, sizeof(path), "%s/voltage_now", dir);
    if (read_file_int(path, &uv) == 0) *mv = uv / 1000;
    snprintf(path, sizeof(path), "%s/current_now", dir);
    if (read_file_int(path, &ua) == 0) *ma = ua / 1000;
    snprintf(path, sizeof(path), "%s/charge_now", dir);
    if (read_file_int(path, &uah) == 0) *mah = uah / 1000;
    snprintf(path, sizeof(path), "%s/status", dir);
    char raw[64];
    if (read_file_str(path, raw, sizeof(raw)) == 0 && scap > 0) {
        // JSON に埋めるので英数と空白以外は落とす
        size_t j = 0;
        for (size_t i = 0; raw[i] && j < scap - 1; i++)
            if (isalnum((unsigned char)raw[i]) || raw[i] == ' ') status[j++] = raw[i];
        status[j] = '\0';
    }
}

static int load_target(void) {
    int v;
    if (read_file_int(TARGET_FILE, &v) != 0) return -1;
    for (int i = 0; i < NCHOICES; i++) if (CHOICES[i] == v) return v;
    return -1;
}

static void save_target(int v) {
    mkdir(STATE_DIR, 0755);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), TARGET_FILE ".tmp");
    FILE *f = fopen(tmp, "w");
    if (!f) { log_msg("save_target fopen failed v=%d errno=%d", v, errno); return; }
    fprintf(f, "%d\n", v);
    fclose(f);
    rename(tmp, TARGET_FILE);
    log_msg("save_target v=%d", v);
}

static int load_interval(void) {
    int v;
    if (read_file_int(INTERVAL_FILE, &v) != 0) return DEFAULT_INTERVAL;
    for (int i = 0; i < NINT; i++) if (INT_CHOICES[i] == v) return v;
    return DEFAULT_INTERVAL;
}

static void save_interval(int v) {
    mkdir(STATE_DIR, 0755);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), INTERVAL_FILE ".tmp");
    FILE *f = fopen(tmp, "w");
    if (!f) { log_msg("save_interval fopen failed v=%d errno=%d", v, errno); return; }
    fprintf(f, "%d\n", v);
    fclose(f);
    rename(tmp, INTERVAL_FILE);
    log_msg("save_interval v=%d", v);
}

static int load_speed(void) {
    int v;
    if (read_file_int(SPEED_FILE, &v) != 0) return 0;  // 無ければ無制限（純正の既定）
    for (int i = 0; i < NSPD; i++) if (SPD_CHOICES[i] == v) return v;
    return 0;
}

static void save_speed(int v) {
    mkdir(STATE_DIR, 0755);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), SPEED_FILE ".tmp");
    FILE *f = fopen(tmp, "w");
    if (!f) { log_msg("save_speed fopen failed v=%d errno=%d", v, errno); return; }
    fprintf(f, "%d\n", v);
    fclose(f);
    rename(tmp, SPEED_FILE);
    log_msg("save_speed v=%d", v);
}

// 充電電流の上限を書く。読み戻し手段が無いので成否は終了コードだけで判定する。
static int apply_speed(int ma) {
    char cmd[160], buf[256];
    if (ma == 0) {
        snprintf(cmd, sizeof(cmd), ECTOOL " chargecurrentlimit %lld", UNLIMITED_MA);
    } else {
        snprintf(cmd, sizeof(cmd), ECTOOL " chargecurrentlimit %d", ma);
    }
    int rc = run_capture(cmd, buf, sizeof(buf));
    if (rc != 0) log_msg("apply_speed FAILED ma=%d rc=%d out=[%s]", ma, rc, buf);
    return rc == 0 ? 0 : -1;
}

// 速度の見回り: 読み戻せないので、保存値を毎周期そのまま書き直す。
// ログは「前回 keeper が書いた値」と変わったときだけ出す（失敗時は毎回出す）。
static void keeper_speed(void) {
    static int last_applied = -2;  // -2 = まだ書いていない
    int cur = load_speed();
    if (apply_speed(cur) == 0) {
        if (cur != last_applied) log_msg("keeper: speed applied (blind) ma=%d", cur);
        last_applied = cur;
    }
}

// 目標を1回適用し、読み戻して照合する。戻り値 0=読み戻し一致、-1=不一致/不明。
static int apply_once(int v) {
    char cmd[160], buf[1024];
    if (v == 100) {
        snprintf(cmd, sizeof(cmd), ECTOOL " chargecontrol normal");
    } else {
        snprintf(cmd, sizeof(cmd), ECTOOL " chargecontrol normal %d %d", v, v);
    }
    int rc = run_capture(cmd, buf, sizeof(buf));
    log_msg("apply cmd=[%s] rc=%d out=[%s]", cmd, rc, buf);
    if (rc != 0) return -1;
    bool on; int lo, hi;
    if (read_sustainer(&on, &lo, &hi) != 0) {
        log_msg("apply verify: readback unknown v=%d", v);
        return -1;
    }
    if (v == 100) {
        if (!on) { log_msg("apply verify OK v=100 (off)"); return 0; }
        log_msg("apply verify MISMATCH v=100 but on %d~%d", lo, hi);
        return -1;
    }
    if (on && lo == v && hi == v) { log_msg("apply verify OK v=%d", v); return 0; }
    log_msg("apply verify MISMATCH want=%d got on=%d %d~%d", v, (int)on, lo, hi);
    return -1;
}

// 適用＋照合、外れたら1回だけ再試行。
static void apply_target(int v) {
    if (apply_once(v) == 0) return;
    log_msg("apply retry once v=%d", v);
    if (apply_once(v) != 0) log_msg("apply FAILED after retry v=%d", v);
}

// 目標と実状態が食い違っていたら入れ直す。読み取り不明のときは何もしない。
static void keeper(void) {
    keeper_speed();
    int target = load_target();
    if (target < 0) return;
    bool on; int lo, hi;
    if (read_sustainer(&on, &lo, &hi) != 0) {
        log_msg("keeper: state unknown, do nothing (target=%d)", target);
        return;
    }
    if (target == 100) {
        if (on) { log_msg("keeper: target=100 but on %d~%d, applying", lo, hi); apply_target(100); }
    } else {
        if (!on || lo != target || hi != target) {
            log_msg("keeper: mismatch target=%d got on=%d %d~%d, applying", target, (int)on, lo, hi);
            apply_target(target);
        }
    }
}

// ---------- 画面 ----------
static const char *PAGE =
"<!DOCTYPE html><html lang='ja'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>充電キープ</title><style>"
"body{font-family:sans-serif;background:#111;color:#eee;margin:24px}"
"h1{font-size:20px} .row{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0}"
"button{font-size:22px;padding:14px 20px;border-radius:10px;border:2px solid #555;"
"background:#222;color:#eee;cursor:pointer}"
"button.active{border-color:#4caf50;background:#1b3a1e;font-weight:bold}"
"button.active::before{content:'● ';color:#4caf50}"
"#st{font-size:16px;line-height:1.7;color:#ccc}"
"h2{font-size:16px;margin:22px 0 0;color:#eee}"
"</style></head><body><h1>充電キープ</h1>"
"<div id='st'>読み取り中…</div>"
"<div class='row' id='btns'></div>"
"<h2>充電速度</h2>"
"<div class='row' id='spdbtns'></div>"
"<h2>見回りの周期</h2>"
"<div class='row' id='ivbtns'></div>"
"<script>"
"const CHOICES=[50,55,60,70,80,100];"
"const SPD_CHOICES=[500,1000,2000,3000,0];"
"const INT_CHOICES=[30,60,300,600];"
"function ivLabel(s){return s<60?s+'秒':(s/60)+'分';}"
"function stLabel(t){return {'Charging':'充電中','Discharging':'放電中','Full':'満充電','Not charging':'充電停止中','Unknown':'不明'}[t]||t||'不明';}"
"function spdLabel(v){return v===0?'無制限':v+'';}"
"const btns=document.getElementById('btns');"
"CHOICES.forEach(n=>{const b=document.createElement('button');b.textContent=n+'%';"
"b.id='b'+n;b.onclick=()=>set(n);btns.appendChild(b);});"
"const ivbtns=document.getElementById('ivbtns');"
"INT_CHOICES.forEach(s=>{const b=document.createElement('button');b.textContent=ivLabel(s);"
"b.id='i'+s;b.onclick=()=>setIv(s);ivbtns.appendChild(b);});"
"const spdbtns=document.getElementById('spdbtns');"
"SPD_CHOICES.forEach(v=>{const b=document.createElement('button');b.textContent=spdLabel(v);"
"b.id='s'+v;b.onclick=()=>setSpd(v);spdbtns.appendChild(b);});"
"async function refresh(){try{const r= await fetch('/api/state');const s= await r.json();"
"document.getElementById('st').innerHTML="
"'現在の電池: '+(s.battery>=0?s.battery+'%':'不明')+'<br>'"
"+'電圧: '+(s.elec?(s.voltage_mv/1000).toFixed(2)+' V':'不明')+'　電流: <span id=cur>'+(s.elec?((s.current_ma>=0?'+':'')+s.current_ma)+' mA':'不明')+'</span>　電力: '+(s.elec?(s.voltage_mv*Math.abs(s.current_ma)/1000000).toFixed(1)+' W':'不明')+'<br>'"
"+'電池の残量: '+(s.charge_mah>=0?s.charge_mah+' mAh':'不明')+'　状態: <span id=bst>'+stLabel(s.bat_status)+'</span><br>'"
"+'保持の状態: '+((s.known===false)?'不明（読み取り失敗）':(s.on?('有効（'+s.lower+'%〜'+s.upper+'%）'):'無効（満充電）'))+'<br>'"
"+'選んでいる値: '+(s.target>0?s.target+'%':'未設定')+'<br>'"
"+'充電速度の上限: '+(s.speed===0?'無制限':s.speed+' mA')+'<br>'"
"+'見回り周期: '+ivLabel(s.interval);"
"const col=s.bat_status==='Charging'?'#4caf50':(s.bat_status==='Discharging'?'#ff9800':'#ccc');"
"const ce=document.getElementById('cur');if(ce)ce.style.color=col;"
"const be=document.getElementById('bst');if(be)be.style.color=col;"
"CHOICES.forEach(n=>{document.getElementById('b'+n).classList.toggle('active',s.target===n);});"
"SPD_CHOICES.forEach(v=>{document.getElementById('s'+v).classList.toggle('active',s.speed===v);});"
"INT_CHOICES.forEach(v=>{document.getElementById('i'+v).classList.toggle('active',s.interval===v);});"
"}catch(e){document.getElementById('st').textContent='本体と通信できません';}}"
"async function set(n){await fetch('/api/set?n='+n,{method:'POST'});refresh();}"
"async function setSpd(v){await fetch('/api/speed?ma='+v,{method:'POST'});refresh();}"
"async function setIv(s){await fetch('/api/interval?sec='+s,{method:'POST'});refresh();}"
"refresh();setInterval(refresh,5000);"
"</script></body></html>";

static void state_json(char *out, size_t cap) {
    bool on; int lo, hi;
    int known = (read_sustainer(&on, &lo, &hi) == 0);
    int batt = read_battery_percent();
    int target = load_target();
    int interval = load_interval();
    int speed = load_speed();
    int mv, ma, mah; char bat_status[32];
    read_battery_elec(&mv, &ma, &mah, bat_status, sizeof(bat_status));
    int elec = (mv >= 0 && ma >= 0) ? 1 : 0;
    // 電流の符号は状態で付け替える（v6）: 充電中=＋、放電中=−
    if (elec && strcmp(bat_status, "Discharging") == 0) ma = -ma;
    snprintf(out, cap,
             "{\"target\":%d,\"known\":%s,\"on\":%s,\"lower\":%d,\"upper\":%d,\"battery\":%d,\"interval\":%d,\"speed\":%d,"
             "\"elec\":%s,\"voltage_mv\":%d,\"current_ma\":%d,\"charge_mah\":%d,\"bat_status\":\"%s\"}",
             target, known ? "true" : "false", on ? "true" : "false", lo, hi, batt, interval, speed,
             elec ? "true" : "false", mv, ma, mah, bat_status);
}

static void respond(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, ctype, strlen(body));
    send(fd, hdr, n, 0);
    send(fd, body, strlen(body), 0);
}

static void handle_client(int fd) {
    char req[4096];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';
    char method[16], path[1024];
    if (sscanf(req, "%15s %1023s", method, path) != 2) return;

    if (strncmp(path, "/api/set", 8) == 0) {
        int v = -1;
        const char *q = strstr(path, "n=");
        if (q) v = atoi(q + 2);
        bool valid = false;
        for (int i = 0; i < NCHOICES; i++) if (CHOICES[i] == v) valid = true;
        if (!valid) { respond(fd, "400 Bad Request", "text/plain", "bad value"); return; }
        log_msg("api/set v=%d", v);
        save_target(v);
        apply_target(v);
        char js[768]; state_json(js, sizeof(js));
        respond(fd, "200 OK", "application/json", js);
        return;
    }
    if (strncmp(path, "/api/speed", 10) == 0) {
        int v = -1;
        const char *q = strstr(path, "ma=");
        if (q) v = atoi(q + 3);
        bool valid = false;
        for (int i = 0; i < NSPD; i++) if (SPD_CHOICES[i] == v) valid = true;
        if (!valid) { respond(fd, "400 Bad Request", "text/plain", "bad value"); return; }
        log_msg("api/speed ma=%d", v);
        save_speed(v);
        apply_speed(v);
        char js[768]; state_json(js, sizeof(js));
        respond(fd, "200 OK", "application/json", js);
        return;
    }
    if (strncmp(path, "/api/interval", 13) == 0) {
        int v = -1;
        const char *q = strstr(path, "sec=");
        if (q) v = atoi(q + 4);
        bool valid = false;
        for (int i = 0; i < NINT; i++) if (INT_CHOICES[i] == v) valid = true;
        if (!valid) { respond(fd, "400 Bad Request", "text/plain", "bad value"); return; }
        log_msg("api/interval sec=%d", v);
        save_interval(v);
        char js[768]; state_json(js, sizeof(js));
        respond(fd, "200 OK", "application/json", js);
        return;
    }
    if (strncmp(path, "/api/state", 10) == 0) {
        char js[768]; state_json(js, sizeof(js));
        respond(fd, "200 OK", "application/json", js);
        return;
    }
    if (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0) {
        respond(fd, "200 OK", "text/html; charset=utf-8", PAGE);
        return;
    }
    respond(fd, "404 Not Found", "text/plain", "not found");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mkdir(STATE_DIR, 0755);
    log_msg("chargectl v7 starting (interval=%d, speed=%d)", load_interval(), load_speed());
    keeper();
    time_t last_keeper = time(NULL);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 8) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "chargectl listening on 127.0.0.1:%d\n", PORT);
    fflush(stderr);

    for (;;) {
        int iv = load_interval();
        long remain = (long)iv - (long)(time(NULL) - last_keeper);
        if (remain <= 0) {
            keeper();
            last_keeper = time(NULL);
            continue;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(srv, &rf);
        struct timeval tv = {(time_t)remain, 0};
        int r = select(srv + 1, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(srv, &rf)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) { handle_client(c); close(c); }
        }
    }
    return 0;
}
