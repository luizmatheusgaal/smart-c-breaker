#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ==========================
// CONFIGURAÇÕES
// ==========================
const char* ssid = "SEU_WIFI_SSID";
const char* password = "SUA_SENHA_WIFI";

const char* USUARIO_WEB = "admin";
const char* SENHA_WEB   = "TROQUE_ESTA_SENHA";
const char* CODIGO_CADASTRO = "TROQUE_O_CODIGO";
const char* NOME_DISPOSITIVO = "SMART C-BREAKER";
const char* CHAVE_DISPOSITIVO = "TROQUE_POR_UMA_CHAVE_LONGA_ALEATORIA";

const char* SUPABASE_URL = "https://SEU-PROJETO.supabase.co";
const char* SUPABASE_KEY = "SUA_CHAVE_PUBLISHABLE_OU_ANON";

#define LM35_PIN  34
#define SCT_PIN   35
#define RELE_PIN  26   // HL-52S IN1 (ativo em LOW)

const float BURDEN_OHMS = 56.0;
const float CALIBRACAO_SCT = 2000.0 / BURDEN_OHMS;
const float TENSAO_REDE = 220.0;

const float LIMITE_CORRENTE_PADRAO = 30.0;
const float LIMITE_TEMPERATURA_PADRAO = 60.0;
const float LIM_I_MIN = 0.5, LIM_I_MAX = 40.0;
const float LIM_T_MIN = 20.0, LIM_T_MAX = 100.0;
float limiteCorrente = LIMITE_CORRENTE_PADRAO;
float limiteTemperatura = LIMITE_TEMPERATURA_PADRAO;
const int   LEITURAS_PARA_DISPARO = 2;
const unsigned long INTERVALO_MS = 1000;

const int HIST_N = 360;
const unsigned long HIST_INTERVALO_MS = 10000;

const unsigned long ENVIO_MS = 30000;

const unsigned long SESSAO_MS = 30UL * 60UL * 1000UL;
const int MAX_FALHAS = 5;
const unsigned long BLOQUEIO_MS = 30000;

WebServer server(80);

float temperatura = 0, corrente = 0, potencia = 0;
double energiaWh = 0;
unsigned long ultimaMedicao = 0;
bool disparado = false;
int leiturasAcima = 0;
int leiturasTempAcima = 0;
const char* motivoDisparo = "";

float histTemp[HIST_N], histCorr[HIST_N];
int histCount = 0, histHead = 0;
unsigned long ultimoHist = 0;
double tempSoma = 0;
int tempN = 0;
float corrPico = 0;

struct Leitura { float t, c, p, e; bool d; };
Leitura pendente;
volatile bool envioPendente = false;
unsigned long ultimoEnvio = 0;
double envSoma = 0;
int envN = 0;
float envPico = 0;

const int MAX_SESSOES = 4;
struct Sessao { String token; String usuario; unsigned long ultima; bool dono; String apiToken; bool acesso; };
Sessao sessoes[MAX_SESSOES];
String usuarioAtual = "";
bool donoAtual = false;
bool acessoAtual = false;
int sessaoAtualIdx = -1;

const unsigned long REGISTRO_MS = 300000;
unsigned long ultimoRegistro = 0;
bool registroOk = true;
String macLocal = "";
bool registrarDispositivo();
unsigned long cadastroLivreEm = 0;
int falhas = 0;
unsigned long bloqueadoAte = 0;

const bool USAR_IP_FIXO = false;
IPAddress IP_FIXO(10, 174, 81, 20);
IPAddress GATEWAY_FIXO(10, 174, 81, 1);
IPAddress MASCARA_FIXA(255, 255, 255, 0);
IPAddress DNS_SECUNDARIO(8, 8, 8, 8);
const char* HOSTNAME = "smart-cbreaker";
const unsigned long HEARTBEAT_MS = 60000;

Preferences prefs;
uint32_t hbAnterior = 0;
bool hbPendente = false;
unsigned long ultimoHeartbeat = 0;
bool ipFixoAtivo = false;

const char* LOG_FILE = "/log.txt";
const size_t LOG_MAX_BYTES = 60000;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
const long FUSO_SEGUNDOS = -3 * 3600;
const unsigned long WIFI_RETRY_MS = 10000;

SemaphoreHandle_t logMutex = NULL;
bool fsOk = false;
bool supabaseOk = true;
bool wifiOk = true;
unsigned long wifiPerdidoEm = 0;
unsigned long wifiUltimaTentativa = 0;

bool horaValida() { return time(nullptr) > 1700000000; }

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "energia ligada";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_PANIC:    return "falha do programa (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "queda de tensao (brownout)";
    case ESP_RST_EXT:      return "botao EN/reset";
    default:               return "desconhecido";
  }
}

void formatarDuracao(unsigned long seg, char* out, size_t n) {
  unsigned long h = seg / 3600, m = (seg % 3600) / 60, s = seg % 60;
  if (h) snprintf(out, n, "%luh %lum %lus", h, m, s);
  else if (m) snprintf(out, n, "%lum %lus", m, s);
  else snprintf(out, n, "%lus", s);
}

void aparaLog() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;
  size_t tam = f.size();
  if (tam <= LOG_MAX_BYTES) { f.close(); return; }
  f.seek(tam - LOG_MAX_BYTES / 2);
  f.readStringUntil('\n');
  File t = LittleFS.open("/log.tmp", "w");
  uint8_t buf[128];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    t.write(buf, n);
  }
  f.close();
  t.close();
  LittleFS.remove(LOG_FILE);
  LittleFS.rename("/log.tmp", LOG_FILE);
}

void logEvento(char nivel, const char* fmt, ...) {
  char msg[80];
  va_list a;
  va_start(a, fmt);
  vsnprintf(msg, sizeof(msg), fmt, a);
  va_end(a);
  for (char* c = msg; *c; c++) {
    if (*c == '\r' || *c == '\n') *c = ' ';
    else if (*c == '|') *c = '/';
  }

  char hora[24];
  if (horaValida()) {
    time_t agora = time(nullptr);
    struct tm tmInfo;
    localtime_r(&agora, &tmInfo);
    strftime(hora, sizeof(hora), "%Y-%m-%d %H:%M:%S", &tmInfo);
  } else {
    snprintf(hora, sizeof(hora), "boot+%lus", millis() / 1000);
  }

  const char* nome = nivel == 'E' ? "ERRO" : (nivel == 'W' ? "AVISO" : "INFO");
  char linha[128];
  snprintf(linha, sizeof(linha), "%s | %s | %s", hora, nome, msg);
  Serial.println(linha);

  if (!fsOk || !logMutex) return;
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  File f = LittleFS.open(LOG_FILE, "a");
  if (f) {
    f.println(linha);
    size_t tam = f.size();
    f.close();
    if (tam > LOG_MAX_BYTES) aparaLog();
  }
  xSemaphoreGive(logMutex);
}

// ==========================
// PÁGINAS
// ==========================
const char LOGIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Login - Monitor ESP32</title>
<style>
  body { margin:0; font-family:Arial,sans-serif; background:#111827; color:white;
         display:flex; justify-content:center; align-items:center; height:100vh; }
  form { background:#1f2937; padding:40px; border-radius:20px; width:280px;
         box-shadow:0 10px 30px rgba(0,0,0,0.4); }
  h1 { text-align:center; margin-top:0; }
  input { width:100%; box-sizing:border-box; padding:12px; margin:8px 0; border:none;
          border-radius:10px; background:#374151; color:white; font-size:16px; }
  button { width:100%; padding:12px; margin-top:12px; border:none; border-radius:10px;
           background:#38bdf8; color:#111827; font-size:16px; font-weight:bold; cursor:pointer; }
  #erro { color:#f87171; text-align:center; margin-top:12px; min-height:20px; font-size:14px; }
  .link { text-align:center; margin-top:14px; font-size:14px; }
  .link a { color:#38bdf8; text-decoration:none; }
</style>
</head>
<body>
<form id="f">
  <h1>Monitor</h1>
  <input id="u" placeholder="Usuário" autocomplete="username" required>
  <input id="p" type="password" placeholder="Senha" autocomplete="current-password" required>
  <button type="submit">Entrar</button>
  <div id="erro"></div>
  <div class="link"><a href="/cadastro">Criar conta</a></div>
</form>
<script>
  document.getElementById('f').addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = new URLSearchParams({
      u: document.getElementById('u').value,
      p: document.getElementById('p').value
    });
    const r = await fetch('/login', { method: 'POST', body });
    if (r.ok) { location.href = '/disjuntores'; return; }
    document.getElementById('erro').innerText =
      r.status === 429 ? 'Muitas tentativas. Aguarde 30 s.' :
      r.status === 503 ? 'Banco de usuários indisponível. Tente novamente.' :
      'Usuário ou senha inválidos.';
  });
</script>
</body>
</html>
)rawliteral";

const char CADASTRO_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cadastro - Monitor ESP32</title>
<style>
  body { margin:0; font-family:Arial,sans-serif; background:#111827; color:white;
         display:flex; justify-content:center; align-items:center; min-height:100vh; }
  form { background:#1f2937; padding:40px; border-radius:20px; width:280px;
         box-shadow:0 10px 30px rgba(0,0,0,0.4); }
  h1 { text-align:center; margin-top:0; }
  input { width:100%; box-sizing:border-box; padding:12px; margin:8px 0; border:none;
          border-radius:10px; background:#374151; color:white; font-size:16px; }
  button { width:100%; padding:12px; margin-top:12px; border:none; border-radius:10px;
           background:#38bdf8; color:#111827; font-size:16px; font-weight:bold; cursor:pointer; }
  #msg { text-align:center; margin-top:12px; min-height:20px; font-size:14px; color:#f87171; }
  #msg.ok { color:#4ade80; }
  .dica { color:#9ca3af; font-size:12px; margin:0 0 6px; }
  .link { text-align:center; margin-top:14px; font-size:14px; }
  .link a { color:#38bdf8; text-decoration:none; }
</style>
</head>
<body>
<form id="f">
  <h1>Criar conta</h1>
  <input id="u" placeholder="Usuário" autocomplete="username" maxlength="20" required>
  <div class="dica">3 a 20 caracteres: letras, números, ponto, hífen e sublinhado.</div>
  <input id="p" type="password" placeholder="Senha" autocomplete="new-password" maxlength="64" required>
  <input id="p2" type="password" placeholder="Confirmar senha" autocomplete="new-password" maxlength="64" required>
  <div class="dica">Senha com no mínimo 6 caracteres.</div>
  <input id="c" placeholder="Código de cadastro" autocomplete="off">
  <button type="submit">Cadastrar</button>
  <div id="msg"></div>
  <div class="link"><a href="/">Voltar ao login</a></div>
</form>
<script>
  const msg = document.getElementById('msg');
  function mostrar(texto, ok) { msg.innerText = texto; msg.className = ok ? 'ok' : ''; }

  document.getElementById('f').addEventListener('submit', async (e) => {
    e.preventDefault();
    const p = document.getElementById('p').value;
    if (p !== document.getElementById('p2').value) { mostrar('As senhas não coincidem.'); return; }
    if (p.length < 6) { mostrar('A senha precisa ter no mínimo 6 caracteres.'); return; }
    const body = new URLSearchParams({
      u: document.getElementById('u').value,
      p: p,
      codigo: document.getElementById('c').value
    });
    mostrar('Enviando...', true);
    try {
      const r = await fetch('/cadastro', { method: 'POST', body });
      if (r.ok) {
        mostrar('Conta criada! Redirecionando para o login...', true);
        setTimeout(() => location.href = '/', 1500);
        return;
      }
      const erros = {
        400: 'Usuário ou senha em formato inválido.',
        403: 'Código de cadastro inválido.',
        409: 'Esse usuário já existe.',
        429: 'Aguarde alguns segundos e tente de novo.',
        503: 'Banco de dados indisponível. Tente novamente.'
      };
      mostrar(erros[r.status] || 'Não foi possível cadastrar.');
    } catch (err) {
      mostrar('Falha de conexão com o dispositivo.');
    }
  });
</script>
</body>
</html>
)rawliteral";

const char DISJUNTORES_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Meus disjuntores - Monitor ESP32</title>
<style>
  body { margin:0; font-family:Arial,sans-serif; background:#111827; color:white; padding:20px; }
  .topo { display:flex; justify-content:space-between; align-items:center; max-width:900px; margin:0 auto 20px; flex-wrap:wrap; gap:10px; }
  h1 { margin:0; }
  .sub { color:#9ca3af; font-size:14px; margin-top:4px; }
  .lista { max-width:900px; margin:0 auto; display:grid; grid-template-columns:repeat(auto-fit,minmax(260px,1fr)); gap:15px; }
  .card { background:#1f2937; border-radius:16px; padding:20px; box-shadow:0 10px 30px rgba(0,0,0,0.3); }
  .card h2 { margin:0 0 6px; font-size:20px; }
  .info { color:#9ca3af; font-size:13px; line-height:1.6; word-break:break-all; }
  .badge { display:inline-block; padding:3px 10px; border-radius:999px; font-size:12px; font-weight:bold; margin-right:6px; }
  .on { background:#065f46; } .off { background:#4b5563; } .aqui { background:#1d4ed8; } .papel { background:#374151; }
  .acoes { margin-top:14px; }
  button { padding:10px 18px; border:none; border-radius:10px; background:#38bdf8; color:#111827; font-weight:bold; cursor:pointer; }
  button.sec { background:#374151; color:white; margin-left:6px; }
  .aviso { max-width:900px; margin:0 auto 15px; padding:12px; border-radius:10px; background:#92400e; font-size:14px; display:none; }
  .vazio { max-width:900px; margin:0 auto; text-align:center; color:#9ca3af; padding:40px 10px; }
  .nota { color:#9ca3af; font-size:12px; margin-top:8px; }
</style>
</head>
<body>
<div class="topo">
  <div>
    <h1>Meus disjuntores</h1>
    <div class="sub">Conta: <span id="usuario">--</span></div>
  </div>
  <div>
    <button class="sec" onclick="carregar()">Atualizar</button>
    <button class="sec" onclick="location.href='/logout'">Sair</button>
  </div>
</div>

<div id="aviso" class="aviso"></div>
<div id="lista" class="lista"></div>
<div id="vazio" class="vazio" style="display:none">
  Nenhum disjuntor associado à sua conta.<br>
  Peça ao responsável para associar sua conta a um disjuntor.
</div>

<script>
  async function api(url) {
    const r = await fetch(url);
    if (r.status === 401) { location.href = '/'; throw new Error('sessao'); }
    return r;
  }

  function el(tag, cls, txt) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt !== undefined) e.textContent = txt;
    return e;
  }

  function desenhar(j, eu) {
    const lista = document.getElementById('lista');
    const vazio = document.getElementById('vazio');
    const aviso = document.getElementById('aviso');
    lista.innerHTML = '';

    if (!j.acesso) {
      aviso.style.display = 'block';
      aviso.textContent = 'Sua conta ainda não está associada a este disjuntor (MAC ' + eu.mac +
                          '). Peça ao responsável para associá-la.';
    } else {
      aviso.style.display = 'none';
    }

    vazio.style.display = j.dispositivos.length ? 'none' : 'block';

    j.dispositivos.forEach(d => {
      const aqui = d.mac === eu.mac;
      const card = el('div', 'card');
      card.appendChild(el('h2', '', d.nome));

      const badges = el('div');
      badges.appendChild(el('span', 'badge ' + (d.online ? 'on' : 'off'), d.online ? 'Online' : 'Offline'));
      if (aqui) badges.appendChild(el('span', 'badge aqui', 'Este dispositivo'));
      badges.appendChild(el('span', 'badge papel', d.papel));
      card.appendChild(badges);

      const info = el('div', 'info');
      info.appendChild(document.createTextNode('MAC: ' + d.mac));
      info.appendChild(document.createElement('br'));
      info.appendChild(document.createTextNode('IP: ' + (d.ip || 'desconhecido')));
      card.appendChild(info);

      const acoes = el('div', 'acoes');
      if (aqui) {
        const b = el('button', '', 'Abrir painel');
        b.onclick = () => { location.href = '/painel'; };
        acoes.appendChild(b);
      } else if (d.ip) {
        const b = el('button', '', 'Abrir painel');
        b.onclick = () => { window.open('http://' + d.ip + '/', '_blank'); };
        acoes.appendChild(b);
        acoes.appendChild(el('div', 'nota', 'Outro dispositivo: será preciso entrar de novo nele.'));
      }
      card.appendChild(acoes);
      lista.appendChild(card);
    });
  }

  async function carregar() {
    try {
      const eu = await (await api('/api/eu')).json();
      document.getElementById('usuario').textContent = eu.usuario + (eu.dono ? ' (dono)' : '');
      const r = await api('/api/disjuntores');
      if (!r.ok) {
        const aviso = document.getElementById('aviso');
        aviso.style.display = 'block';
        aviso.textContent = 'Não foi possível carregar a lista de disjuntores agora.';
        return;
      }
      desenhar(await r.json(), eu);
    } catch (e) { console.error(e); }
  }

  carregar();
  setInterval(carregar, 30000);
</script>
</body>
</html>
)rawliteral";

const char DASH_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Monitor ESP32</title>
<style>
  body { margin:0; font-family:Arial,sans-serif; background:#111827; color:white; padding:20px; }
  .topo { display:flex; justify-content:space-between; align-items:center; max-width:900px; margin:0 auto 20px; }
  h1 { margin:0; }
  .grid { max-width:900px; margin:0 auto; display:grid;
          grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:15px; }
  .card { background:#1f2937; border-radius:16px; padding:20px; text-align:center;
          box-shadow:0 10px 30px rgba(0,0,0,0.3); }
  .rotulo { color:#9ca3af; font-size:14px; }
  .valor { font-size:36px; font-weight:bold; margin-top:6px; }
  .temp { color:#38bdf8; } .corr { color:#facc15; } .pot { color:#f87171; } .ener { color:#4ade80; }
  .estado { max-width:900px; margin:15px auto; padding:12px; border-radius:10px;
            text-align:center; font-weight:bold; background:#065f46; }
  .estado.disparo { background:#991b1b; }
  .grafico { max-width:900px; margin:15px auto; background:#1f2937; border-radius:16px; padding:15px; }
  .grafico-topo { display:flex; justify-content:space-between; align-items:center; margin-bottom:10px; }
  select { padding:8px 12px; border:none; border-radius:8px; background:#374151; color:white; }
  canvas { width:100%; height:320px; display:block; }
  .botoes { max-width:900px; margin:0 auto; text-align:center; }
  button { margin:5px; padding:10px 20px; border:none; border-radius:10px;
           background:#374151; color:white; cursor:pointer; }
  button:hover { background:#4b5563; }
  .limites { max-width:900px; margin:15px auto; background:#1f2937; border-radius:16px; padding:15px; }
  .limites-titulo { margin-bottom:10px; }
  .limites-campos { display:flex; flex-wrap:wrap; gap:12px; align-items:flex-end; }
  .limites-campos label { display:flex; flex-direction:column; font-size:14px; color:#9ca3af; gap:4px; }
  .limites-campos input { width:130px; padding:8px 10px; border:none; border-radius:8px;
                          background:#374151; color:white; font-size:16px; }
  .limites-campos input:disabled, .limites-campos button:disabled { opacity:0.5; cursor:not-allowed; }
  .limAviso { display:none; margin-top:8px; font-size:13px; color:#facc15; }
  #limMsg { margin-top:8px; min-height:18px; font-size:14px; color:#f87171; }
  #limMsg.ok { color:#4ade80; }
</style>
</head>
<body>
<div class="topo">
  <h1>Monitor</h1>
  <div>
    <button onclick="location.href='/disjuntores'">Disjuntores</button>
    <button onclick="location.href='/logs'">Logs</button>
    <button onclick="location.href='/logout'">Sair</button>
  </div>
</div>

<div class="grid">
  <div class="card"><div class="rotulo">Temperatura</div>
    <div class="valor temp"><span id="temperatura">--</span> °C</div></div>
  <div class="card"><div class="rotulo">Corrente</div>
    <div class="valor corr"><span id="corrente">--</span> A</div></div>
  <div class="card"><div class="rotulo">Potência aparente</div>
    <div class="valor pot"><span id="potencia">--</span> VA</div></div>
  <div class="card"><div class="rotulo">Consumo</div>
    <div class="valor ener"><span id="energia">--</span> kWh</div></div>
</div>

<div id="estado" class="estado">Circuito: --</div>

<div class="limites">
  <div class="limites-titulo">Limites de desarme</div>
  <div class="limites-campos">
    <label>Corrente (A)
      <input id="limI" type="number" step="0.1" min="0.5" max="40" inputmode="decimal">
    </label>
    <label>Temperatura (°C)
      <input id="limT" type="number" step="1" min="20" max="100" inputmode="decimal">
    </label>
    <button id="limSalvar" onclick="salvarLimites()">Salvar limites</button>
  </div>
  <div id="limAviso" class="limAviso">Somente o dono (conta administradora) pode alterar os limites.</div>
  <div id="limMsg"></div>
</div>

<div class="grafico">
  <div class="grafico-topo">
    <span>Histórico</span>
    <select id="faixa" onchange="mudarFaixa()">
      <option value="live">Ao vivo (1 h)</option>
      <option value="6">Últimas 6 h</option>
      <option value="24">Últimas 24 h</option>
      <option value="168">Últimos 7 dias</option>
    </select>
  </div>
  <canvas id="grafico"></canvas>
</div>

<div class="botoes">
  <button onclick="rearmar()">Rearmar</button>
  <button onclick="zerar()">Zerar consumo</button>
</div>

<script>
  let limite = 30;
  let cfg = null;
  let ultimo = null;
  let limitesSujos = false;
  ['limI', 'limT'].forEach(id =>
    document.getElementById(id).addEventListener('input', () => { limitesSujos = true; }));

  async function salvarLimites() {
    const msg = document.getElementById('limMsg');
    if (!confirm('Alterar os limites de desarme?')) return;
    const body = new URLSearchParams({
      corrente: document.getElementById('limI').value,
      temperatura: document.getElementById('limT').value
    });
    try {
      const r = await api('/limites', { method: 'POST', body });
      const j = await r.json();
      if (r.ok) {
        limitesSujos = false;
        msg.className = 'ok';
        msg.innerText = j.alterado ? 'Limites salvos e registrados no log.' : 'Nenhuma alteração.';
        atualizar();
      } else {
        msg.className = '';
        msg.innerText = j.erro || 'Valores inválidos.';
      }
    } catch (e) {
      if (e.message !== 'sessao') { msg.className = ''; msg.innerText = 'Falha ao salvar.'; }
    }
  }

  async function api(url, opts) {
    const r = await fetch(url, opts);
    if (r.status === 401) { location.reload(); throw new Error('sessao'); }
    return r;
  }

  async function atualizar() {
    try {
      const d = await (await api('/dados')).json();
      limite = d.limite;
      if (!limitesSujos) {
        document.getElementById('limI').value = d.limite;
        document.getElementById('limT').value = d.limite_temperatura;
      }
      ['limI', 'limT', 'limSalvar'].forEach(id => document.getElementById(id).disabled = !d.dono);
      document.getElementById('limAviso').style.display = d.dono ? 'none' : 'block';
      document.getElementById('temperatura').innerText = d.temperatura.toFixed(1);
      document.getElementById('corrente').innerText = d.corrente.toFixed(2);
      document.getElementById('potencia').innerText = d.potencia.toFixed(0);
      document.getElementById('energia').innerText = d.energia_kwh.toFixed(3);
      const e = document.getElementById('estado');
      e.innerText = d.disparado ? 'Circuito ABERTO' + (d.motivo ? ' (' + d.motivo + ')' : '') : 'Circuito fechado (normal)';
      e.className = d.disparado ? 'estado disparo' : 'estado';
    } catch (e) { console.error(e); }
  }

  async function carregarGrafico() {
    const faixa = document.getElementById('faixa').value;
    try {
      let h;
      if (faixa === 'live') {
        const r = await (await api('/historico')).json();
        const agora = Date.now() / 1000, n = r.temp.length;
        h = { ts: r.temp.map((_, i) => agora - (n - 1 - i) * r.intervalo), temp: r.temp, corr: r.corr };
      } else {
        if (!cfg) cfg = await (await api('/config')).json();
        const headers = { 'Content-Type': 'application/json', 'apikey': cfg.key };
        if (cfg.key.startsWith('eyJ')) headers['Authorization'] = 'Bearer ' + cfg.key;
        const r = await fetch(cfg.url + '/rest/v1/rpc/historico', {
          method: 'POST', headers, body: JSON.stringify({ horas: parseInt(faixa), pontos: 120 })
        });
        if (!r.ok) throw new Error('supabase ' + r.status);
        h = await r.json();
      }
      ultimo = h;
      desenhar(h);
    } catch (e) {
      if (e.message !== 'sessao') desenhar(null, 'Não foi possível carregar o histórico');
      console.error(e);
    }
  }

  function mudarFaixa() { carregarGrafico(); }

  function formatarHora(ts, longo) {
    const d = new Date(ts * 1000);
    const hora = d.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' });
    if (!longo) return hora;
    return d.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit' }) + ' ' + hora;
  }

  function desenhar(h, msg) {
    const cv = document.getElementById('grafico');
    const dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    cv.width = W * dpr; cv.height = H * dpr;
    const c = cv.getContext('2d');
    c.scale(dpr, dpr);
    c.clearRect(0, 0, W, H);

    const L = 50, R = 50, T = 25, B = 35;
    const w = W - L - R, hh = H - T - B;
    const n = h ? h.temp.length : 0;

    c.font = '12px Arial';
    if (n < 2) {
      c.fillStyle = '#9ca3af'; c.textAlign = 'center';
      c.fillText(msg || 'Coletando dados...', W / 2, H / 2);
      return;
    }

    const t0 = h.ts[0], t1 = h.ts[n - 1];
    const span = Math.max(t1 - t0, 1);

    let tMin = Math.min(...h.temp) - 1, tMax = Math.max(...h.temp) + 1;
    if (tMax - tMin < 5) { const m = (tMax + tMin) / 2; tMin = m - 2.5; tMax = m + 2.5; }
    const cMax = Math.max(Math.max(...h.corr) * 1.2, 5);

    const x = ts => L + ((ts - t0) / span) * w;
    const yT = v => T + hh - ((v - tMin) / (tMax - tMin)) * hh;
    const yC = v => T + hh - (v / cMax) * hh;

    c.strokeStyle = '#374151'; c.lineWidth = 1;
    c.textAlign = 'right';
    for (let k = 0; k <= 4; k++) {
      const y = T + (hh / 4) * k;
      c.beginPath(); c.moveTo(L, y); c.lineTo(L + w, y); c.stroke();
      c.fillStyle = '#38bdf8';
      c.fillText((tMax - (tMax - tMin) * k / 4).toFixed(1), L - 6, y + 4);
    }
    c.textAlign = 'left';
    for (let k = 0; k <= 4; k++) {
      const y = T + (hh / 4) * k;
      c.fillStyle = '#facc15';
      c.fillText((cMax - cMax * k / 4).toFixed(1), L + w + 6, y + 4);
    }

    c.textAlign = 'center'; c.fillStyle = '#9ca3af';
    for (let k = 0; k <= 4; k++) {
      const ts = t0 + span * k / 4;
      c.fillText(formatarHora(ts, span > 90000), x(ts), H - 12);
    }

    if (limite <= cMax) {
      c.strokeStyle = '#ef4444'; c.setLineDash([6, 6]);
      c.beginPath(); c.moveTo(L, yC(limite)); c.lineTo(L + w, yC(limite)); c.stroke();
      c.setLineDash([]);
      c.fillStyle = '#ef4444'; c.textAlign = 'left';
      c.fillText('limite ' + limite + ' A', L + 4, yC(limite) - 4);
    }

    function linha(arr, y, cor) {
      c.strokeStyle = cor; c.lineWidth = 2; c.beginPath();
      arr.forEach((v, i) => i ? c.lineTo(x(h.ts[i]), y(v)) : c.moveTo(x(h.ts[i]), y(v)));
      c.stroke();
    }
    linha(h.temp, yT, '#38bdf8');
    linha(h.corr, yC, '#facc15');

    c.textAlign = 'left'; c.font = '13px Arial';
    c.fillStyle = '#38bdf8'; c.fillText('● Temperatura (°C, média)', L, 14);
    c.fillStyle = '#facc15'; c.fillText('● Corrente (A, pico)', L + 190, 14);
  }

  async function zerar() {
    if (!confirm('Zerar o consumo acumulado?')) return;
    await api('/reset', { method: 'POST' });
    atualizar();
  }
  async function rearmar() {
    if (!confirm('Rearmar e fechar o circuito?')) return;
    await api('/rearmar', { method: 'POST' });
    atualizar();
  }

  window.addEventListener('resize', () => { if (ultimo) desenhar(ultimo); });
  atualizar(); carregarGrafico();
  setInterval(atualizar, 2000);
  setInterval(carregarGrafico, 15000);
</script>
</body>
</html>
)rawliteral";

const char LOGS_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Logs - Monitor ESP32</title>
<style>
  body { margin:0; font-family:Arial,sans-serif; background:#111827; color:white; padding:20px; }
  .topo { display:flex; justify-content:space-between; align-items:center; max-width:900px; margin:0 auto 20px; }
  h1 { margin:0; }
  .painel { max-width:900px; margin:0 auto; background:#1f2937; border-radius:16px; padding:15px; }
  .barra { display:flex; justify-content:space-between; align-items:center; margin-bottom:10px; gap:10px; flex-wrap:wrap; }
  select, button { padding:8px 14px; border:none; border-radius:8px; background:#374151; color:white; cursor:pointer; }
  button:hover { background:#4b5563; }
  table { width:100%; border-collapse:collapse; font-size:14px; }
  th { text-align:left; color:#9ca3af; padding:8px; border-bottom:1px solid #374151; }
  td { padding:8px; border-bottom:1px solid #2b3544; vertical-align:top; }
  .nivel { font-weight:bold; width:70px; }
  .I { color:#38bdf8; } .W { color:#facc15; } .E { color:#f87171; }
  .vazio { text-align:center; color:#9ca3af; padding:30px; }
  .rodape { color:#9ca3af; font-size:13px; margin-top:10px; }
</style>
</head>
<body>
<div class="topo">
  <h1>Logs</h1>
  <div>
    <button onclick="location.href='/painel'">Painel</button>
    <button onclick="location.href='/disjuntores'">Disjuntores</button>
    <button onclick="location.href='/logout'">Sair</button>
  </div>
</div>

<div class="painel">
  <div class="barra">
    <select id="filtro" onchange="desenhar()">
      <option value="I">Todos</option>
      <option value="W">Avisos e erros</option>
      <option value="E">Somente erros</option>
    </select>
    <button onclick="carregar()">Atualizar</button>
  </div>
  <table>
    <thead><tr><th>Data/hora</th><th>Nível</th><th>Evento</th></tr></thead>
    <tbody id="corpo"></tbody>
  </table>
  <div class="rodape">Log gravado na memória flash (LittleFS), mantido após reiniciar. Atualização a cada 5 s.</div>
</div>

<script>
  let dados = [];
  const nomes = { I: 'INFO', W: 'AVISO', E: 'ERRO' };
  const ordem = { I: 0, W: 1, E: 2 };

  async function carregar() {
    try {
      const r = await fetch('/log');
      if (r.status === 401) { location.reload(); return; }
      const txt = await r.text();
      const letras = { INFO: 'I', AVISO: 'W', ERRO: 'E' };
      dados = txt.split('\n').filter(x => x.trim()).map(linha => {
        const p = linha.split(' | ');
        return { t: p[0], n: letras[p[1]] || 'I', m: p.slice(2).join(' | ') };
      }).reverse();
      desenhar();
    } catch (e) { console.error(e); }
  }

  function desenhar() {
    const min = ordem[document.getElementById('filtro').value];
    const lista = dados.filter(l => ordem[l.n] >= min);
    const corpo = document.getElementById('corpo');
    if (!lista.length) {
      corpo.innerHTML = '<tr><td colspan="3" class="vazio">Nenhum evento</td></tr>';
      return;
    }
    corpo.innerHTML = '';
    lista.forEach(l => {
      const tr = document.createElement('tr');
      const dt = l.t;
      [[dt, ''], [nomes[l.n], 'nivel ' + l.n], [l.m, '']].forEach(([txt, cls]) => {
        const td = document.createElement('td');
        td.textContent = txt; td.className = cls;
        tr.appendChild(td);
      });
      corpo.appendChild(tr);
    });
  }

  carregar();
  setInterval(carregar, 5000);
</script>
</body>
</html>
)rawliteral";

// ==========================
// SENSORES
// ==========================
void abrirCircuito()  { digitalWrite(RELE_PIN, LOW); }
void fecharCircuito() { digitalWrite(RELE_PIN, HIGH); }

float lerTemperatura() {
  long soma = 0;
  for (int i = 0; i < 16; i++) soma += analogReadMilliVolts(LM35_PIN);
  return (soma / 16.0) / 10.0;
}

float lerCorrente() {
  const unsigned long duracaoUs = 200000;
  double soma = 0, somaQuad = 0;
  long n = 0;

  unsigned long inicio = micros();
  while (micros() - inicio < duracaoUs) {
    double mv = analogReadMilliVolts(SCT_PIN);
    soma += mv;
    somaQuad += mv * mv;
    n++;
  }

  double media = soma / n;
  double variancia = somaQuad / n - media * media;
  if (variancia < 0) variancia = 0;

  float i = (sqrt(variancia) / 1000.0) * CALIBRACAO_SCT;
  if (i < 0.3) i = 0;
  return i;
}

void registrarHistorico() {
  histTemp[histHead] = tempN ? tempSoma / tempN : temperatura;
  histCorr[histHead] = corrPico;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
  tempSoma = 0; tempN = 0; corrPico = 0;
}

// ==========================
// SUPABASE (envio em tarefa separada)
// ==========================
void enviarSupabase(const Leitura& l) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(client, String(SUPABASE_URL) + "/rest/v1/leituras");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ"))
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
  http.addHeader("Prefer", "return=minimal");

  String body = "{";
  body += "\"temperatura\":" + String(l.t, 2) + ",";
  body += "\"corrente\":" + String(l.c, 2) + ",";
  body += "\"potencia\":" + String(l.p, 1) + ",";
  body += "\"energia_kwh\":" + String(l.e, 4) + ",";
  body += "\"disparado\":" + String(l.d ? "true" : "false");
  body += "}";

  int code = http.POST(body);
  http.end();

  if (code >= 200 && code < 300) {
    if (!supabaseOk) logEvento('I', "Supabase: envio normalizado");
    supabaseOk = true;
  } else {
    logEvento('E', "Supabase: falha no envio (HTTP %d)", code);
    supabaseOk = false;
  }
}

void restaurarEnergia() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.begin(client, String(SUPABASE_URL) + "/rest/v1/rpc/ultima_energia");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ"))
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

  int code = http.POST("{}");
  if (code == 200) {
    String r = http.getString();
    r.trim();
    if (r.length() && r != "null") {
      energiaWh = r.toDouble() * 1000.0;
      logEvento('I', "Consumo restaurado: %.4f kWh", energiaWh / 1000.0);
    } else {
      logEvento('W', "Supabase sem historico de consumo");
    }
  } else {
    logEvento('E', "Falha ao restaurar consumo (HTTP %d)", code);
  }
  http.end();
}

void tarefaEnvio(void*) {
  for (;;) {
    if (envioPendente) {
      Leitura l = pendente;
      envioPendente = false;
      enviarSupabase(l);
    }
    if (millis() - ultimoRegistro >= REGISTRO_MS) {
      ultimoRegistro = millis();
      registrarDispositivo();
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

void agendarEnvio(float t, float c, float p, float e, bool d) {
  if (envioPendente) return;
  pendente = {t, c, p, e, d};
  envioPendente = true;
}

void atualizarMedicoes() {
  unsigned long agora = millis();
  if (agora - ultimaMedicao < INTERVALO_MS) return;

  float dtHoras = (agora - ultimaMedicao) / 3600000.0;
  ultimaMedicao = agora;

  temperatura = lerTemperatura();
  corrente = lerCorrente();
  potencia = corrente * TENSAO_REDE;
  energiaWh += potencia * dtHoras;

  tempSoma += temperatura; tempN++;
  if (corrente > corrPico) corrPico = corrente;
  if (agora - ultimoHist >= HIST_INTERVALO_MS) {
    ultimoHist = agora;
    registrarHistorico();
  }

  envSoma += temperatura; envN++;
  if (corrente > envPico) envPico = corrente;
  if (agora - ultimoEnvio >= ENVIO_MS) {
    ultimoEnvio = agora;
    agendarEnvio(envSoma / envN, envPico, envPico * TENSAO_REDE,
                 energiaWh / 1000.0, disparado);
    envSoma = 0; envN = 0; envPico = 0;
  }

  if (!disparado) {
    leiturasAcima = (corrente >= limiteCorrente) ? leiturasAcima + 1 : 0;
    leiturasTempAcima = (temperatura >= limiteTemperatura) ? leiturasTempAcima + 1 : 0;
    if (leiturasAcima >= LEITURAS_PARA_DISPARO) {
      disparado = true;
      motivoDisparo = "sobrecorrente";
      abrirCircuito();
      logEvento('E', "SOBRECORRENTE %.1f A (limite %.1f A). Circuito aberto", corrente, limiteCorrente);
      agendarEnvio(temperatura, corrente, potencia, energiaWh / 1000.0, true);
    } else if (leiturasTempAcima >= LEITURAS_PARA_DISPARO) {
      disparado = true;
      motivoDisparo = "temperatura";
      abrirCircuito();
      logEvento('E', "TEMPERATURA %.1f C (limite %.1f C). Circuito aberto", temperatura, limiteTemperatura);
      agendarEnvio(temperatura, corrente, potencia, energiaWh / 1000.0, true);
    }
  }
}

// ==========================
// AUTENTICAÇÃO
// ==========================
int buscarSessao() {
  String cookie = server.header("Cookie");
  for (int i = 0; i < MAX_SESSOES; i++) {
    if (sessoes[i].token.length() == 0) continue;
    if (millis() - sessoes[i].ultima > SESSAO_MS) { sessoes[i].token = ""; continue; }
    if (cookie.indexOf("sid=" + sessoes[i].token) >= 0) return i;
  }
  return -1;
}

bool autenticado() {
  int i = buscarSessao();
  if (i < 0) return false;
  sessoes[i].ultima = millis();
  usuarioAtual = sessoes[i].usuario;
  donoAtual = sessoes[i].dono;
  acessoAtual = sessoes[i].acesso;
  sessaoAtualIdx = i;
  return true;
}

bool exigirSessao() {
  if (autenticado()) return true;
  server.send(401, "application/json", "{\"erro\":\"nao autenticado\"}");
  return false;
}

bool exigirAuth() {
  if (!exigirSessao()) return false;
  if (acessoAtual) return true;
  server.send(403, "application/json", "{\"erro\":\"sem acesso a este disjuntor\"}");
  return false;
}

String gerarToken() {
  char buf[33];
  snprintf(buf, sizeof(buf), "%08x%08x%08x%08x",
           esp_random(), esp_random(), esp_random(), esp_random());
  return String(buf);
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

bool usuarioValido(const String& u) {
  if (u.length() < 3 || u.length() > 20) return false;
  for (size_t i = 0; i < u.length(); i++) {
    char c = u[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') return false;
  }
  return true;
}

int rpcSupabase(const char* fn, const String& body, String& resposta) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(client, String(SUPABASE_URL) + "/rest/v1/rpc/" + fn);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ"))
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

  int code = http.POST(body);
  resposta = code > 0 ? http.getString() : "";
  http.end();
  resposta.trim();
  return code;
}

String jsonValor(const String& js, const char* chave) {
  int i = js.indexOf("\"" + String(chave) + "\"");
  if (i < 0) return "";
  i = js.indexOf(':', i);
  if (i < 0) return "";
  i++;
  while (i < (int)js.length() && js[i] == ' ') i++;
  if (i < (int)js.length() && js[i] == '"') {
    int f = js.indexOf('"', i + 1);
    return f < 0 ? "" : js.substring(i + 1, f);
  }
  int f = i;
  while (f < (int)js.length() && js[f] != ',' && js[f] != '}' && js[f] != ' ') f++;
  return js.substring(i, f);
}

bool registrarDispositivo() {
  static bool primeiro = true;
  String r;
  String body = "{\"p_mac\":\"" + macLocal + "\",\"p_nome\":\"" + jsonEscape(NOME_DISPOSITIVO) +
                "\",\"p_ip\":\"" + WiFi.localIP().toString() +
                "\",\"p_chave\":\"" + jsonEscape(CHAVE_DISPOSITIVO) + "\"}";
  int code = rpcSupabase("registrar_dispositivo", body, r);
  bool ok = code == 200 && r.length() > 0 && r != "null";
  if (ok && primeiro) logEvento('I', "Dispositivo registrado no banco (id %s)", r.c_str());
  else if (ok && !registroOk) logEvento('I', "Registro do dispositivo normalizado");
  else if (!ok && registroOk) logEvento('W', "Registro do dispositivo recusado ou falhou (HTTP %d)", code);
  if (ok) primeiro = false;
  registroOk = ok;
  return ok;
}

String criarSessao(const String& usuario, bool dono, const String& apiToken, bool acesso) {
  int alvo = 0;
  unsigned long maiorIdade = 0;
  for (int i = 0; i < MAX_SESSOES; i++) {
    if (sessoes[i].token.length() == 0) { alvo = i; break; }
    unsigned long idade = millis() - sessoes[i].ultima;
    if (idade >= maiorIdade) { maiorIdade = idade; alvo = i; }
  }
  sessoes[alvo].token = gerarToken();
  sessoes[alvo].usuario = usuario;
  sessoes[alvo].dono = dono;
  sessoes[alvo].apiToken = apiToken;
  sessoes[alvo].acesso = acesso;
  sessoes[alvo].ultima = millis();
  return sessoes[alvo].token;
}

void handleCadastroPagina() {
  server.send_P(200, "text/html", CADASTRO_PAGE);
}

void handleCadastro() {
  String ip = server.client().remoteIP().toString();
  if (millis() < cadastroLivreEm) {
    server.send(429, "application/json", "{\"erro\":\"aguarde\"}");
    return;
  }
  cadastroLivreEm = millis() + 5000;

  String u = server.arg("u"), p = server.arg("p"), codigo = server.arg("codigo");
  if (strlen(CODIGO_CADASTRO) > 0 && codigo != CODIGO_CADASTRO) {
    logEvento('W', "Cadastro negado: codigo invalido (%s)", ip.c_str());
    delay(500);
    server.send(403, "application/json", "{\"erro\":\"codigo\"}");
    return;
  }
  if (!usuarioValido(u) || p.length() < 6 || p.length() > 64) {
    server.send(400, "application/json", "{\"erro\":\"invalido\"}");
    return;
  }
  if (u.equalsIgnoreCase(USUARIO_WEB)) {
    server.send(409, "application/json", "{\"erro\":\"existe\"}");
    return;
  }

  String r;
  int code = rpcSupabase("registrar_usuario",
                         "{\"p_usuario\":\"" + jsonEscape(u) + "\",\"p_senha\":\"" + jsonEscape(p) + "\"}", r);
  if (code != 200) {
    logEvento('E', "Falha ao cadastrar usuario (HTTP %d)", code);
    server.send(503, "application/json", "{\"erro\":\"indisponivel\"}");
  } else if (r == "\"ok\"") {
    logEvento('I', "Usuario cadastrado: %s (%s)", u.c_str(), ip.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
  } else if (r == "\"existe\"") {
    server.send(409, "application/json", "{\"erro\":\"existe\"}");
  } else {
    server.send(400, "application/json", "{\"erro\":\"invalido\"}");
  }
}

void handleLogin() {
  if (millis() < bloqueadoAte) {
    server.send(429, "application/json", "{\"erro\":\"bloqueado\"}");
    return;
  }
  String u = server.arg("u"), p = server.arg("p");
  String ip = server.client().remoteIP().toString();
  bool ok = false, indisponivel = false, dono = false, acesso = false;
  String apiToken = "";

  if (u == USUARIO_WEB && p == SENHA_WEB) {
    ok = true;
    dono = true;
    acesso = true;
  } else if (usuarioValido(u) && p.length() >= 1 && p.length() <= 64) {
    String r;
    int code = rpcSupabase("login_usuario",
                           "{\"p_usuario\":\"" + jsonEscape(u) + "\",\"p_senha\":\"" + jsonEscape(p) +
                           "\",\"p_mac\":\"" + macLocal + "\"}", r);
    if (code == 200) {
      if (r != "null" && r.length() > 2) {
        apiToken = jsonValor(r, "token");
        acesso = jsonValor(r, "acesso") == "true";
        ok = apiToken.length() > 0;
      }
    } else {
      indisponivel = true;
      logEvento('E', "Falha ao validar usuario (HTTP %d)", code);
    }
  }

  if (ok) {
    logEvento('I', "Login realizado: %s (%s)", u.c_str(), ip.c_str());
    falhas = 0;
    if (!acesso) logEvento('W', "Usuario %s sem associacao a este disjuntor", u.c_str());
    String token = criarSessao(u, dono, apiToken, acesso);
    server.sendHeader("Set-Cookie", "sid=" + token + "; Path=/; HttpOnly; SameSite=Strict");
    server.send(200, "application/json", "{\"ok\":true}");
  } else if (indisponivel) {
    server.send(503, "application/json", "{\"erro\":\"indisponivel\"}");
  } else {
    logEvento('W', "Login invalido de %s (%s)", u.c_str(), ip.c_str());
    if (++falhas >= MAX_FALHAS) {
      bloqueadoAte = millis() + BLOQUEIO_MS;
      falhas = 0;
      logEvento('W', "Login bloqueado por %lu s (muitas tentativas)", BLOQUEIO_MS / 1000);
    }
    delay(500);
    server.send(401, "application/json", "{\"erro\":\"invalido\"}");
  }
}

void handleLogout() {
  int i = buscarSessao();
  if (i >= 0) {
    logEvento('I', "Logout: %s", sessoes[i].usuario.c_str());
    sessoes[i].token = "";
    sessoes[i].apiToken = "";
  }
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ==========================
// ROTAS
// ==========================
void redirecionar(const char* destino) {
  server.sendHeader("Location", destino);
  server.send(302, "text/plain", "");
}

void handleRoot() {
  if (autenticado()) redirecionar("/disjuntores");
  else server.send_P(200, "text/html", LOGIN_PAGE);
}

void handlePainel() {
  if (!autenticado()) redirecionar("/");
  else if (!acessoAtual) redirecionar("/disjuntores");
  else server.send_P(200, "text/html", DASH_PAGE);
}

void handleDisjuntoresPagina() {
  if (!autenticado()) redirecionar("/");
  else server.send_P(200, "text/html", DISJUNTORES_PAGE);
}

void handleApiEu() {
  if (!exigirSessao()) return;
  String json = "{\"usuario\":\"" + jsonEscape(usuarioAtual) + "\",\"dono\":" + (donoAtual ? "true" : "false") +
                ",\"mac\":\"" + macLocal + "\",\"acesso\":" + (acessoAtual ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleApiDisjuntores() {
  if (!exigirSessao()) return;
  int idx = sessaoAtualIdx;

  if (donoAtual) {
    String json = "{\"acesso\":true,\"dispositivos\":[{\"id\":0,\"nome\":\"" + jsonEscape(NOME_DISPOSITIVO) +
                  "\",\"mac\":\"" + macLocal + "\",\"ip\":\"" + WiFi.localIP().toString() +
                  "\",\"papel\":\"dono\",\"online\":true}]}";
    server.send(200, "application/json", json);
    return;
  }

  String r;
  int code = rpcSupabase("listar_dispositivos",
                         "{\"p_token\":\"" + jsonEscape(sessoes[idx].apiToken) + "\",\"p_mac\":\"" + macLocal + "\"}", r);
  if (code != 200) {
    logEvento('E', "Falha ao listar disjuntores (HTTP %d)", code);
    server.send(503, "application/json", "{\"erro\":\"indisponivel\"}");
    return;
  }
  if (r == "null" || r.length() < 3) {
    sessoes[idx].token = "";
    server.send(401, "application/json", "{\"erro\":\"sessao expirada\"}");
    return;
  }
  sessoes[idx].acesso = jsonValor(r, "acesso") == "true";
  server.send(200, "application/json", r);
}

void handleLogsPagina() {
  if (!autenticado()) redirecionar("/");
  else if (!acessoAtual) redirecionar("/disjuntores");
  else server.send_P(200, "text/html", LOGS_PAGE);
}

void handleLog() {
  if (!exigirAuth()) return;
  if (!fsOk || xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    server.send(200, "text/plain; charset=utf-8", "");
    return;
  }
  File f = LittleFS.open(LOG_FILE, "r");
  if (f) {
    server.streamFile(f, "text/plain; charset=utf-8");
    f.close();
  } else {
    server.send(200, "text/plain; charset=utf-8", "");
  }
  xSemaphoreGive(logMutex);
}

void handleDados() {
  if (!exigirAuth()) return;
  String json = "{";
  json += "\"temperatura\":" + String(temperatura, 2) + ",";
  json += "\"corrente\":" + String(corrente, 2) + ",";
  json += "\"potencia\":" + String(potencia, 1) + ",";
  json += "\"energia_kwh\":" + String(energiaWh / 1000.0, 4) + ",";
  json += "\"limite\":" + String(limiteCorrente, 1) + ",";
  json += "\"limite_temperatura\":" + String(limiteTemperatura, 1) + ",";
  json += "\"dono\":" + String(donoAtual ? "true" : "false") + ",";
  json += "\"motivo\":\"" + String(motivoDisparo) + "\",";
  json += "\"disparado\":" + String(disparado ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  if (!exigirAuth()) return;
  bool conectado = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"wifi\":\"" + String(conectado ? "conectado" : "desconectado") + "\",";
  json += "\"ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"gateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  json += "\"mascara\":\"" + WiFi.subnetMask().toString() + "\",";
  json += "\"rssi_dbm\":" + String(WiFi.RSSI()) + ",";
  json += "\"hostname\":\"" + String(HOSTNAME) + "\",";
  json += "\"uptime_s\":" + String(millis() / 1000) + ",";
  json += "\"circuito\":\"" + String(disparado ? "aberto" : "fechado") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleHistorico() {
  if (!exigirAuth()) return;
  String t, c;
  t.reserve(histCount * 7 + 4);
  c.reserve(histCount * 7 + 4);
  int inicio = (histHead - histCount + HIST_N) % HIST_N;
  for (int k = 0; k < histCount; k++) {
    int i = (inicio + k) % HIST_N;
    if (k) { t += ","; c += ","; }
    t += String(histTemp[i], 1);
    c += String(histCorr[i], 2);
  }
  String json = "{\"intervalo\":" + String(HIST_INTERVALO_MS / 1000) +
                ",\"temp\":[" + t + "],\"corr\":[" + c + "]}";
  server.send(200, "application/json", json);
}

void handleConfig() {
  if (!exigirAuth()) return;
  String json = "{\"url\":\"" + String(SUPABASE_URL) + "\",\"key\":\"" + String(SUPABASE_KEY) + "\"}";
  server.send(200, "application/json", json);
}

void handleReset() {
  if (!exigirAuth()) return;
  energiaWh = 0;
  logEvento('I', "Consumo acumulado zerado por %s", usuarioAtual.c_str());
  agendarEnvio(temperatura, corrente, potencia, 0, disparado);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLimites() {
  if (!exigirAuth()) return;
  if (!donoAtual) {
    logEvento('W', "Alteracao de limites negada para %s (nao e o dono)", usuarioAtual.c_str());
    server.send(403, "application/json", "{\"erro\":\"Somente o dono pode alterar os limites.\"}");
    return;
  }
  String sc = server.arg("corrente"), st = server.arg("temperatura");
  sc.replace(',', '.');
  st.replace(',', '.');
  float c = sc.toFloat(), t = st.toFloat();

  if (sc.length() == 0 || st.length() == 0 ||
      !(c >= LIM_I_MIN && c <= LIM_I_MAX) || !(t >= LIM_T_MIN && t <= LIM_T_MAX)) {
    char msg[110];
    snprintf(msg, sizeof(msg), "{\"erro\":\"Use corrente de %.1f a %.0f A e temperatura de %.0f a %.0f C\"}",
             LIM_I_MIN, LIM_I_MAX, LIM_T_MIN, LIM_T_MAX);
    server.send(400, "application/json", msg);
    return;
  }

  bool mudouI = fabsf(c - limiteCorrente) >= 0.05f;
  bool mudouT = fabsf(t - limiteTemperatura) >= 0.05f;
  if (mudouI) {
    logEvento('I', "Limite de corrente alterado por %s: %.1f -> %.1f A", usuarioAtual.c_str(), limiteCorrente, c);
    limiteCorrente = c;
    prefs.putFloat("lim_i", c);
  }
  if (mudouT) {
    logEvento('I', "Limite de temperatura alterado por %s: %.1f -> %.1f C", usuarioAtual.c_str(), limiteTemperatura, t);
    limiteTemperatura = t;
    prefs.putFloat("lim_t", t);
  }
  if (mudouI || mudouT) {
    leiturasAcima = 0;
    leiturasTempAcima = 0;
  }

  String json = "{\"ok\":true,\"alterado\":" + String((mudouI || mudouT) ? "true" : "false") +
                ",\"limite\":" + String(limiteCorrente, 1) +
                ",\"limite_temperatura\":" + String(limiteTemperatura, 1) + "}";
  server.send(200, "application/json", json);
}

void handleRearmar() {
  if (!exigirAuth()) return;
  disparado = false;
  motivoDisparo = "";
  leiturasAcima = 0;
  leiturasTempAcima = 0;
  fecharCircuito();
  logEvento('I', "Sistema rearmado por %s. Circuito fechado", usuarioAtual.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRele() {
  if (!exigirAuth()) return;
  String estado = server.arg("estado");
  if (estado == "abrir") {
    disparado = true;
    motivoDisparo = "comando remoto";
    abrirCircuito();
    logEvento('I', "Comando remoto de %s: circuito aberto", usuarioAtual.c_str());
  } else if (estado == "fechar") {
    disparado = false;
    motivoDisparo = "";
    leiturasAcima = 0;
    leiturasTempAcima = 0;
    fecharCircuito();
    logEvento('I', "Comando remoto de %s: circuito fechado", usuarioAtual.c_str());
  } else {
    server.send(400, "application/json", "{\"erro\":\"use estado=abrir ou estado=fechar\"}");
    return;
  }
  server.send(200, "application/json", String("{\"circuito\":\"") + (disparado ? "aberto" : "fechado") + "\"}");
}

void verificarTempoForaDoAr() {
  if (!hbPendente || !horaValida()) return;
  hbPendente = false;
  long fora = (long)time(nullptr) - (long)hbAnterior;
  if (fora < 0) return;
  char dur[24], desde[24];
  formatarDuracao(fora, dur, sizeof(dur));
  time_t t = hbAnterior;
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  strftime(desde, sizeof(desde), "%Y-%m-%d %H:%M:%S", &tmInfo);
  logEvento('W', "Fora do ar por cerca de %s (ultimo sinal %s)", dur, desde);
}

void atualizarHeartbeat() {
  if (millis() - ultimoHeartbeat < HEARTBEAT_MS) return;
  ultimoHeartbeat = millis();
  if (horaValida()) prefs.putUInt("hb", (uint32_t)time(nullptr));
  verificarTempoForaDoAr();
}

// ==========================
// SETUP / LOOP
// ==========================
void setup() {
  digitalWrite(RELE_PIN, HIGH);
  pinMode(RELE_PIN, OUTPUT);
  digitalWrite(RELE_PIN, HIGH);

  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(LM35_PIN, INPUT);
  pinMode(SCT_PIN, INPUT);

  logMutex = xSemaphoreCreateMutex();
  fsOk = LittleFS.begin(true);
  if (!fsOk) Serial.println("ERRO: falha ao montar LittleFS");

  prefs.begin("cbreaker", false);
  limiteCorrente = prefs.getFloat("lim_i", LIMITE_CORRENTE_PADRAO);
  limiteTemperatura = prefs.getFloat("lim_t", LIMITE_TEMPERATURA_PADRAO);
  if (!(limiteCorrente >= LIM_I_MIN && limiteCorrente <= LIM_I_MAX)) limiteCorrente = LIMITE_CORRENTE_PADRAO;
  if (!(limiteTemperatura >= LIM_T_MIN && limiteTemperatura <= LIM_T_MAX)) limiteTemperatura = LIMITE_TEMPERATURA_PADRAO;
  hbAnterior = prefs.getUInt("hb", 0);
  hbPendente = hbAnterior > 1700000000;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (USAR_IP_FIXO) {
    ipFixoAtivo = WiFi.config(IP_FIXO, GATEWAY_FIXO, MASCARA_FIXA, GATEWAY_FIXO, DNS_SECUNDARIO);
  }
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao Wi-Fi");

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++tentativas == 40 && ipFixoAtivo) {
      Serial.println("\nSem conexao com IP fixo; voltando para DHCP");
      WiFi.disconnect();
      WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
      ipFixoAtivo = false;
      WiFi.begin(ssid, password);
    }
    if (tentativas > 80) ESP.restart();
  }

  Serial.println();

  configTime(FUSO_SEGUNDOS, 0, NTP_SERVER_1, NTP_SERVER_2);
  for (int i = 0; i < 20 && !horaValida(); i++) delay(250);

  logEvento('I', "Sistema iniciado (%s)", motivoReinicio());
  if (!horaValida()) logEvento('W', "NTP indisponivel; usando tempo desde o boot");
  logEvento('I', "Wi-Fi conectado, IP %s (%s)", WiFi.localIP().toString().c_str(),
            ipFixoAtivo ? "fixo" : "DHCP");
  if (USAR_IP_FIXO && !ipFixoAtivo)
    logEvento('W', "IP fixo indisponivel nesta rede; usando DHCP");
  logEvento('I', "MAC %s | gateway %s | mascara %s", WiFi.macAddress().c_str(),
            WiFi.gatewayIP().toString().c_str(), WiFi.subnetMask().toString().c_str());

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    logEvento('I', "Acesso por nome: http://%s.local", HOSTNAME);
  } else {
    logEvento('W', "mDNS indisponivel");
  }
  verificarTempoForaDoAr();

  macLocal = WiFi.macAddress();
  registrarDispositivo();
  ultimoRegistro = millis();
  restaurarEnergia();

  const char* headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/", handleRoot);
  server.on("/painel", handlePainel);
  server.on("/disjuntores", handleDisjuntoresPagina);
  server.on("/api/eu", handleApiEu);
  server.on("/api/disjuntores", handleApiDisjuntores);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/cadastro", HTTP_GET, handleCadastroPagina);
  server.on("/cadastro", HTTP_POST, handleCadastro);
  server.on("/logout", handleLogout);
  server.on("/dados", handleDados);
  server.on("/sensores", handleDados);
  server.on("/status", handleStatus);
  server.on("/historico", handleHistorico);
  server.on("/logs", handleLogsPagina);
  server.on("/log", handleLog);
  server.on("/config", handleConfig);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/rearmar", HTTP_POST, handleRearmar);
  server.on("/limites", HTTP_POST, handleLimites);
  server.on("/rele", HTTP_POST, handleRele);
  server.begin();
  Serial.println("Servidor HTTP iniciado!");

  xTaskCreatePinnedToCore(tarefaEnvio, "envio", 8192, NULL, 1, NULL, 0);

  ultimaMedicao = millis();
  ultimoHist = millis();
  ultimoEnvio = millis();
  ultimoHeartbeat = millis();
  if (horaValida()) prefs.putUInt("hb", (uint32_t)time(nullptr));
}

void loop() {
  server.handleClient();
  atualizarMedicoes();
  atualizarHeartbeat();

  bool wifiAgora = WiFi.status() == WL_CONNECTED;
  if (wifiAgora != wifiOk) {
    wifiOk = wifiAgora;
    if (wifiOk) {
      logEvento('I', "Wi-Fi reconectado apos %lu s, IP %s",
                (millis() - wifiPerdidoEm) / 1000, WiFi.localIP().toString().c_str());
    } else {
      wifiPerdidoEm = millis();
      wifiUltimaTentativa = millis();
      logEvento('W', "Wi-Fi desconectado; tentando reconectar");
    }
  }

  if (!wifiOk && millis() - wifiUltimaTentativa >= WIFI_RETRY_MS) {
    wifiUltimaTentativa = millis();
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
}
