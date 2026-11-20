# SMART C-BREAKER

Disjuntor inteligente de prototipo (PUC-PR, Conectividade em Sistemas Ciberfisicos). Um ESP32 mede corrente e temperatura, abre um rele ao detectar sobrecorrente ou temperatura excessiva, e atua como servidor HTTP na rede local, com painel web protegido por login, log persistente e historico opcional no Supabase.

> **Aviso:** projeto didatico, nao certificado. Nao substitui disjuntor, DR ou qualquer protecao exigida pela ABNT NBR 5410. Os contatos do modulo rele (cerca de 10 A) nao suportam correntes altas: o rele deve comandar a bobina de um contator dimensionado para a carga. Instalacao em 220 V deve ser feita por profissional habilitado.

## Componentes

| Item | Funcao |
|---|---|
| ESP32-WROOM-32D | Controlador, Wi-Fi e servidor HTTP |
| SCT-013-000 (100 A / 50 mA) | Sensor de corrente nao invasivo |
| LM35 | Sensor de temperatura (10 mV/C) |
| Modulo rele 5 V (HL-52S, ativo em LOW) | Atuador |
| 2 x resistor 10 kOhm | Divisor de polarizacao (1,65 V) do SCT-013 |
| 1 x resistor 56 Ohm | Resistor de carga (burden) do SCT-013 |
| 1 x capacitor 10 uF | Filtro do ponto de polarizacao |
| Protoboard e jumpers | Montagem (o modulo rele usa jumper entre JD-VCC e VCC, sem fonte propria) |

## Ligacoes

| ESP32 | Liga em |
|---|---|
| GPIO 34 | Saida (Vout) do LM35 |
| GPIO 35 | Fio 2 do SCT-013 (e uma ponta do resistor de 56 Ohm) |
| GPIO 26 | IN1 do modulo rele |
| VIN (5 V) | +Vs do LM35 e VCC do modulo rele (com jumper JD-VCC/VCC) |
| 3V3 | Resistor de 10 kOhm (R1) do divisor |
| GND | GND comum de todos os componentes |

Condicionamento do SCT-013-000:

```
3V3 --[10k]--+--[10k]-- GND
             |
             +--[10uF]-- GND   (positivo no ponto central)
             |
             +-- fio 1 do SCT
fio 2 do SCT -- GPIO 35
[56 Ohm] entre o fio 1 e o fio 2 do SCT
```

O ponto central deve medir cerca de 1,65 V sem corrente no cabo. O clamp envolve **apenas um condutor** (fase ou neutro). O esquema completo esta em `esquema_eletrico.svg`.

## Configuracao do firmware

1. Instale o suporte a ESP32 na Arduino IDE. Escolha um esquema de particao com LittleFS/SPIFFS (o padrao de 4 MB serve).
2. Abra `smart_c_breaker/smart_c_breaker.ino` e edite as constantes do topo:

```cpp
const char* ssid = "SEU_WIFI_SSID";
const char* password = "SUA_SENHA_WIFI";
const char* USUARIO_WEB = "admin";
const char* SENHA_WEB   = "TROQUE_ESTA_SENHA";
const char* CODIGO_CADASTRO = "TROQUE_O_CODIGO";
const char* CHAVE_DISPOSITIVO = "TROQUE_POR_UMA_CHAVE_LONGA_ALEATORIA";
const char* SUPABASE_URL = "https://SEU-PROJETO.supabase.co";
const char* SUPABASE_KEY = "SUA_CHAVE_PUBLISHABLE_OU_ANON";
```

   `USUARIO_WEB`/`SENHA_WEB` e a conta administradora local (funciona mesmo sem internet). `CODIGO_CADASTRO` e o codigo exigido para criar contas novas em `/cadastro`: **troque-o** e informe apenas a quem deve poder controlar o rele. Com valor vazio (`""`), qualquer pessoa na rede poderia se cadastrar.

3. Ajuste, se necessario: `BURDEN_OHMS` (56) e `TENSAO_REDE` (220 V). Os limites de desarme padrao sao `LIMITE_CORRENTE_PADRAO` (30 A) e `LIMITE_TEMPERATURA_PADRAO` (60 C), e podem ser alterados depois pelo painel web (ficam salvos na flash).
4. Grave na placa e abra o Serial Monitor (115200). O IP, o MAC e o gateway aparecem no log de inicializacao. O IP e atribuido por DHCP. O dispositivo tambem responde em `http://smart-cbreaker.local`.

**Nunca publique credenciais reais.** Use a chave publica (`publishable` ou `anon`) do Supabase e nunca a `service_role` ou `secret`.

## Rotas HTTP (porta 80)

Todas, exceto `/`, `/login` e `/cadastro`, exigem login (cookie de sessao). Sao aceitas ate 4 sessoes simultaneas.

| Metodo | Caminho | Funcao |
|---|---|---|
| GET | `/` | Tela de login (se ja logado, redireciona para `/disjuntores`) |
| GET | `/disjuntores` | Pagina com os disjuntores associados a conta do usuario |
| GET | `/painel` | Painel do disjuntor (exige conta associada a ele, ou o dono) |
| GET | `/api/eu`, `/api/disjuntores` | Dados da conta e lista de disjuntores (JSON) |
| POST | `/login` | Autenticacao (`u`, `p`): conta local ou usuario do Supabase |
| GET | `/cadastro` | Pagina de cadastro de usuario |
| POST | `/cadastro` | Cria usuario (`u`, `p`, `codigo`) |
| GET | `/logout` | Encerra a sessao |
| GET | `/dados` ou `/sensores` | Temperatura, corrente, potencia aparente, consumo e estado |
| GET | `/status` | IP, MAC, Wi-Fi, sinal (RSSI), gateway, uptime e estado do circuito |
| POST | `/rele` | `estado=abrir` ou `estado=fechar` |
| POST | `/rearmar` | Rearma apos disparo da protecao |
| POST | `/limites` | Altera os limites de desarme (`corrente` de 0,5 a 40 A, `temperatura` de 20 a 100 C); salva na flash e registra no log. **Somente o dono** (conta administradora local, `USUARIO_WEB`); os demais recebem 403 |
| POST | `/reset` | Zera o consumo acumulado |
| GET | `/historico` | Historico em memoria (1 hora) |
| GET | `/logs` | Pagina de logs |
| GET | `/log` | Conteudo do log (texto), gravado em LittleFS |
| GET | `/config` | URL e chave do Supabase para o painel |

Exemplo:

```
curl.exe -c cookie.txt -d "u=admin&p=SUA_SENHA" http://IP_DO_ESP32/login
curl.exe -b cookie.txt http://IP_DO_ESP32/status
curl.exe -b cookie.txt -d "estado=abrir" http://IP_DO_ESP32/rele
```

## Funcionamento

- Mede temperatura e corrente a cada 1 s (corrente RMS por amostragem de 200 ms).
- Abre o rele se a corrente ou a temperatura ficarem acima do limite por 2 leituras seguidas. O estado fica travado ate o rearme.
- Log em `/log.txt` (LittleFS) com data e hora via NTP, niveis INFO, AVISO e ERRO, mantido apos reiniciar. Registra quedas de Wi-Fi, motivo do reinicio e tempo fora do ar.
- Envia leituras ao Supabase a cada 30 s, em tarefa separada, e restaura o consumo acumulado ao ligar.

## Supabase

1. Crie um projeto em supabase.com.
2. No **SQL Editor**, execute:

```sql
create table if not exists leituras (
  id bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  temperatura real,
  corrente real,
  potencia real,
  energia_kwh real,
  disparado boolean
);

create index if not exists leituras_created_at_idx on leituras (created_at);

alter table leituras enable row level security;

drop policy if exists "esp32 pode inserir" on leituras;
create policy "esp32 pode inserir"
  on leituras for insert to anon
  with check (true);

create or replace function historico(horas int default 24, pontos int default 120)
returns json
language sql
stable
security definer
set search_path = public
as $$
  with par as (
    select greatest(horas, 1) as h,
           (greatest(horas, 1)::numeric * 3600) / least(greatest(pontos, 10), 300) as step
  ),
  b as (
    select floor(extract(epoch from l.created_at) / par.step)::bigint as k,
           avg(l.temperatura) as t,
           max(l.corrente) as c
    from leituras l, par
    where l.created_at > now() - make_interval(hours => par.h)
    group by 1
  )
  select json_build_object(
    'ts',   coalesce(json_agg(round(b.k * par.step) order by b.k), '[]'::json),
    'temp', coalesce(json_agg(round(b.t::numeric, 1) order by b.k), '[]'::json),
    'corr', coalesce(json_agg(round(b.c::numeric, 2) order by b.k), '[]'::json)
  )
  from b, par;
$$;

grant execute on function historico(int, int) to anon;

create or replace function ultima_energia()
returns real
language sql
stable
security definer
set search_path = public
as $$
  select energia_kwh from leituras order by created_at desc limit 1;
$$;

grant execute on function ultima_energia() to anon;
```

3. Para o cadastro de usuarios, execute tambem:

```sql
create extension if not exists pgcrypto with schema extensions;

create table if not exists usuarios (
  id bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  usuario text not null unique,
  senha_hash text not null
);

alter table usuarios enable row level security;

create or replace function registrar_usuario(p_usuario text, p_senha text)
returns text
language plpgsql
security definer
set search_path = public, extensions
as $$
declare u text := lower(trim(p_usuario));
begin
  if u !~ '^[a-z0-9_.-]{3,20}$' or length(p_senha) < 6 or length(p_senha) > 64 then
    return 'invalido';
  end if;
  insert into usuarios (usuario, senha_hash) values (u, crypt(p_senha, gen_salt('bf')));
  return 'ok';
exception when unique_violation then
  return 'existe';
end;
$$;

create or replace function validar_login(p_usuario text, p_senha text)
returns boolean
language sql
stable
security definer
set search_path = public, extensions
as $$
  select exists (
    select 1 from usuarios
    where usuario = lower(trim(p_usuario))
      and senha_hash = crypt(p_senha, senha_hash)
  );
$$;

revoke all on function registrar_usuario(text, text) from public;
revoke all on function validar_login(text, text) from public;
grant execute on function registrar_usuario(text, text) to anon;
grant execute on function validar_login(text, text) to anon;
```

   As senhas ficam apenas como hash bcrypt na tabela `usuarios`, que nao tem politicas de leitura (a chave publica nao consegue le-la). Para listar ou remover usuarios, use o **Table Editor** do painel do Supabase.

4. Para dispositivos e vinculo com usuarios (relacao muitos para muitos), execute (depois do passo 3):

```sql
create table if not exists dispositivos (
  id bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  mac text not null unique check (mac ~ '^[0-9A-F]{2}(:[0-9A-F]{2}){5}$'),
  nome text not null default 'SMART C-BREAKER',
  ultimo_contato timestamptz
);

create table if not exists usuarios_dispositivos (
  usuario_id bigint not null references usuarios(id) on delete cascade,
  dispositivo_id bigint not null references dispositivos(id) on delete cascade,
  papel text not null default 'membro' check (papel in ('admin', 'membro')),
  created_at timestamptz not null default now(),
  primary key (usuario_id, dispositivo_id)
);

create index if not exists usuarios_dispositivos_disp_idx on usuarios_dispositivos (dispositivo_id);

alter table leituras add column if not exists dispositivo_id bigint references dispositivos(id) on delete set null;
create index if not exists leituras_disp_data_idx on leituras (dispositivo_id, created_at);

alter table dispositivos enable row level security;
alter table usuarios_dispositivos enable row level security;

create or replace function usuario_tem_acesso(p_usuario text, p_dispositivo_id bigint)
returns boolean
language sql
stable
security definer
set search_path = public
as $$
  select exists (
    select 1
    from usuarios_dispositivos ud
    join usuarios u on u.id = ud.usuario_id
    where u.usuario = lower(trim(p_usuario))
      and ud.dispositivo_id = p_dispositivo_id
  );
$$;

revoke all on function usuario_tem_acesso(text, bigint) from public;
grant execute on function usuario_tem_acesso(text, bigint) to anon;
```

   Relacao: um usuario pode acessar varios dispositivos e um dispositivo pode ter varios usuarios (tabela de ligacao `usuarios_dispositivos`, com `papel` admin ou membro). O vinculo **nao** e exposto a chave publica (qualquer um poderia se dar acesso): crie-o pelo painel do Supabase, por exemplo:

```sql
insert into usuarios_dispositivos (usuario_id, dispositivo_id, papel)
select u.id, d.id, 'admin'
from usuarios u, dispositivos d
where u.usuario = 'ana' and d.mac = 'AA:BB:CC:DD:EE:FF';

-- quem acessa cada dispositivo
select d.nome, d.mac, u.usuario, ud.papel
from usuarios_dispositivos ud
join usuarios u on u.id = ud.usuario_id
join dispositivos d on d.id = ud.dispositivo_id
order by d.nome, u.usuario;
```

5. Para a pagina de disjuntores do usuario (login com sessao no banco e registro do dispositivo), execute tambem:

```sql
alter table dispositivos add column if not exists ip text;
alter table dispositivos add column if not exists chave_hash text;

drop function if exists registrar_dispositivo(text, text);

create table if not exists sessoes_api (
  token text primary key,
  usuario_id bigint not null references usuarios(id) on delete cascade,
  criado_em timestamptz not null default now(),
  expira_em timestamptz not null
);
create index if not exists sessoes_api_expira_idx on sessoes_api (expira_em);
alter table sessoes_api enable row level security;

create or replace function registrar_dispositivo(p_mac text, p_nome text, p_ip text, p_chave text)
returns bigint
language plpgsql
security definer
set search_path = public, extensions
as $$
declare m text := upper(trim(p_mac));
        d dispositivos%rowtype;
        r bigint;
begin
  if m !~ '^[0-9A-F]{2}(:[0-9A-F]{2}){5}$' or length(coalesce(p_chave, '')) < 16 then
    return null;
  end if;
  if p_ip is not null and p_ip !~ '^[0-9a-fA-F:.]{3,45}$' then
    return null;
  end if;

  select * into d from dispositivos where mac = m;
  if not found then
    insert into dispositivos (mac, nome, ip, chave_hash, ultimo_contato)
    values (m, coalesce(nullif(trim(p_nome), ''), 'SMART C-BREAKER'), p_ip,
            crypt(p_chave, gen_salt('bf')), now())
    returning id into r;
    return r;
  end if;

  if d.chave_hash is not null and d.chave_hash <> crypt(p_chave, d.chave_hash) then
    return null;
  end if;

  update dispositivos
     set ip = p_ip,
         chave_hash = coalesce(d.chave_hash, crypt(p_chave, gen_salt('bf'))),
         ultimo_contato = now()
   where id = d.id;
  return d.id;
end;
$$;

create or replace function login_usuario(p_usuario text, p_senha text, p_mac text default null)
returns jsonb
language plpgsql
security definer
set search_path = public, extensions
as $$
declare u usuarios%rowtype;
        t text;
        tem_acesso boolean;
begin
  select * into u from usuarios
   where usuario = lower(trim(p_usuario))
     and senha_hash = crypt(p_senha, senha_hash);
  if not found then
    return null;
  end if;

  delete from sessoes_api where expira_em < now();
  t := encode(gen_random_bytes(24), 'hex');
  insert into sessoes_api (token, usuario_id, expira_em) values (t, u.id, now() + interval '2 hours');

  tem_acesso := exists (
    select 1 from usuarios_dispositivos ud
    join dispositivos d on d.id = ud.dispositivo_id
    where ud.usuario_id = u.id and d.mac = upper(trim(coalesce(p_mac, '')))
  );
  return jsonb_build_object('token', t, 'acesso', tem_acesso);
end;
$$;

create or replace function listar_dispositivos(p_token text, p_mac text default null)
returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare uid bigint;
        m text := upper(trim(coalesce(p_mac, '')));
        res jsonb;
begin
  select usuario_id into uid from sessoes_api where token = p_token and expira_em > now();
  if uid is null then
    return null;
  end if;

  select jsonb_build_object(
           'acesso', exists (
             select 1 from usuarios_dispositivos x
             join dispositivos dd on dd.id = x.dispositivo_id
             where x.usuario_id = uid and dd.mac = m),
           'dispositivos', coalesce(jsonb_agg(jsonb_build_object(
             'id', d.id, 'nome', d.nome, 'mac', d.mac, 'ip', d.ip, 'papel', ud.papel,
             'online', coalesce(d.ultimo_contato > now() - interval '10 minutes', false)
           ) order by d.nome), '[]'::jsonb))
    into res
    from usuarios_dispositivos ud
    join dispositivos d on d.id = ud.dispositivo_id
   where ud.usuario_id = uid;
  return res;
end;
$$;

revoke all on function registrar_dispositivo(text, text, text, text) from public;
revoke all on function login_usuario(text, text, text) from public;
revoke all on function listar_dispositivos(text, text) from public;
grant execute on function registrar_dispositivo(text, text, text, text) to anon;
grant execute on function login_usuario(text, text, text) to anon;
grant execute on function listar_dispositivos(text, text) to anon;
```

   O ESP32 se registra no banco ao ligar (e a cada 5 min) usando o MAC e a `CHAVE_DISPOSITIVO` do firmware; a primeira chave registrada para um MAC passa a ser a unica aceita para atualiza-lo (guardada como hash). Depois que o dispositivo aparecer na tabela `dispositivos`, associe os usuarios a ele com o `insert` do passo 4. Usuarios sem associacao entram, veem a lista vazia e nao acessam o painel desse disjuntor. O dono (conta local) sempre acessa.

6. Em **Project Settings > API**, copie a **Project URL** e a chave **publishable/anon** para o firmware.

Com essas politicas, a chave publica so consegue inserir leituras e chamar as duas funcoes, que devolvem dados agregados. A tabela nao fica legivel diretamente.

## Limitacoes conhecidas

- O HTTP trafega sem criptografia (use apenas em rede local confiavel): usuario e senha de login e de cadastro passam em texto puro na rede local.
- Login e cadastro consultam o Supabase por HTTPS sem validar o certificado (`setInsecure`), e bloqueiam o servidor por ate 4 s se o banco estiver lento. Sem internet, so a conta administradora local consegue entrar.
- A potencia e aparente (corrente x 220 V fixos), nao ha medicao de tensao nem de fator de potencia.
- O tempo de atuacao da protecao e de cerca de 1 a 3 s, e nao substitui a protecao contra curto-circuito.
- A rota `/config` entrega a chave publica do Supabase ao navegador logado.
- Se o ESP32 perder energia, o rele desliga e a carga volta a conduzir, sem protecao.
