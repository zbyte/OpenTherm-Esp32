# Свой Mosquitto перед ThingsBoard (docker compose)

Зачем: связь платы напрямую с ThingsBoard Cloud (EU) у домашнего провайдера «замерзает» — после
нескольких килобайт от сервера TCP-соединение перестаёт получать данные, MQTT отваливается
каждые 1–2 минуты, обновление прошивки идёт рывками. Плата подключается к своему брокеру
(ближе и без такой фильтрации), а он мостом (bridge) пересылает её топики в ThingsBoard.

```
плата ──MQTT 1883──> Mosquitto (свой сервер) ──bridge, TLS 8883──> mqtt.eu.thingsboard.cloud
```

Прошивка не меняется: топики те же (`v1/devices/me/...`, `v2/fw/...`), имя пользователя —
токен устройства; добавляется только пароль к своему брокеру (`tb pass`).

Файлы — в `server/mosquitto/`:

| Файл | Что |
|---|---|
| `docker-compose.yml` | контейнер `eclipse-mosquitto:2.0.20`, порт 1883 |
| `config/mosquitto.conf` | брокер: только по паролю, логи в stdout |
| `config/acl.example` | доступ: токену — только топики ThingsBoard; копия с токеном — `config/acl` |
| `config/bridge.conf.example` | мост в ThingsBoard; копия с токеном — `config/conf.d/bridge.conf` |

Один мост = одно устройство ThingsBoard (мост подключается его токеном). Для второй платы —
второй блок `connection` со своим токеном и своими префиксами топиков; пока не нужно.

## 1. Сервер

Нужны Docker с плагином compose и открытый входящий порт 1883/tcp.

Если сервер в России, «заморозка» может переехать с платы на мост. Поэтому после запуска
(шаг 2) мост стоит понаблюдать полчаса, ещё до переключения платы: в `docker compose logs`
не должно быть повторных `Connecting bridge thingsboard` каждые пару минут.

## 2. Развернуть

Скопировать каталог `server/mosquitto` на сервер (например, в `/opt/mosquitto`), затем:

```sh
cd /opt/mosquitto
mkdir -p config/conf.d data

# токен устройства из ThingsBoard (карточка устройства → «Копировать токен»)
TOKEN='<токен>'
# мост и доступ: подставить токен вместо DEVICE_ACCESS_TOKEN
sed "s/DEVICE_ACCESS_TOKEN/$TOKEN/" config/bridge.conf.example > config/conf.d/bridge.conf
sed "s/DEVICE_ACCESS_TOKEN/$TOKEN/" config/acl.example > config/acl

# пароль для платы: имя пользователя = тот же токен, пароль — придумать
docker run --rm -v "$PWD/config:/mosquitto/config" eclipse-mosquitto:2.0.20 \
  mosquitto_passwd -c -b /mosquitto/config/passwd "$TOKEN" '<пароль>'
sudo chown 1883:1883 config/passwd config/acl && sudo chmod 0700 config/passwd config/acl

docker compose up -d
docker compose logs -f
```

В логе должно быть:

```
Opening ipv4 listen socket on port 1883.
Connecting bridge thingsboard (mqtt.eu.thingsboard.cloud:8883)
```

и никаких `Error` / `Connection refused`. Если мост не поднимается по TLS — переключить его на
1883 без шифрования (комментарий в `bridge.conf`).

Пока плата не переключена, ThingsBoard будет показывать устройство «активным» из‑за моста —
так и дальше: активность теперь означает связь моста, а связь платы видно по свежести
телеметрии.

## 3. Проверить брокер

С сервера, своим паролем:

```sh
docker compose exec mosquitto mosquitto_sub -u '<токен>' -P '<пароль>' -t 'v1/devices/me/#' -v
```

Во втором окне отправить тестовую телеметрию — она должна появиться и в ThingsBoard
(вкладка «Последняя телеметрия» устройства):

```sh
docker compose exec mosquitto mosquitto_pub -u '<токен>' -P '<пароль>' \
  -t v1/devices/me/telemetry -m '{"bridge_test":1}'
```

## 4. Переключить плату

По сети (с ПК в домашней сети):

```powershell
.\build.ps1 cmd "tb pass <пароль>"
.\build.ps1 cmd "tb host <адрес сервера> 1883"
.\build.ps1 cmd "tb"        # online=1, pass=set
```

или на веб-странице платы, форма «Подключение»: сервер, порт 1883, «Пароль MQTT». Токен не
меняется. Пароль и токен в лог не пишутся.

Если что-то не так — вернуть прямое подключение: `tb host mqtt.eu.thingsboard.cloud 8883` и
`tb pass -` (пароль ThingsBoard не нужен).

## 5. Проверить работу через мост

- В ThingsBoard идёт телеметрия, в логе платы нет `No PING_RESP` каждые 1–2 минуты.
- Команда с панели (например, `set_temp`) доходит: в логе платы `cloud: shared set_temp applied`.
- Общие атрибуты не «отражаются» обратно: среди **клиентских** атрибутов устройства в
  ThingsBoard не должно появиться ключей `set_*` (Mosquitto подписывается на локальную сторону
  моста с флагом no-local, так что входящее сообщение назад не уходит — но проверить стоит).
- Обновление прошивки (docs/thingsboard.md) идёт без переподключений: в `tb` у загрузки
  `0 reconnects`, 1 МБ — за минуты.

## Обслуживание

```sh
docker compose logs --since 1h      # лог
docker compose restart              # после правки конфигурации
docker compose pull && docker compose up -d   # обновить образ (сменив тег в docker-compose.yml)
```

Сменить пароль платы: заново `mosquitto_passwd -b ... passwd '<токен>' '<новый>'`,
`docker compose restart`, на плате `tb pass <новый>`. Сменили токен в ThingsBoard — поменять
его в `bridge.conf`, `acl`, в `passwd` (имя пользователя) и на плате (`tb token`).
