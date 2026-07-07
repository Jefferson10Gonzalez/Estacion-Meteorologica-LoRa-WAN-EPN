# Suscripción MQTT a TTN y envío de métricas a Grafana Cloud via Prometheus remote_write
# Dependencias: paho-mqtt, requests, python-snappy, protobuf

import json
import time
import logging
import struct
import snappy
import requests
import paho.mqtt.client as mqtt
from datetime import datetime, timezone

# ─────────────────────────────────────────────
#  TTN MQTT
# ─────────────────────────────────────────────
TTN_BROKER    = "<TTN_BROKER>"           # Ej: eu1.cloud.thethings.network
TTN_PORT      = <TTN_PORT>               # Puerto MQTT (usualmente 1883 o 8883)
TTN_APP_ID    = "lora-weather-station-2026"
TTN_API_KEY   = "<TTN_API_KEY>"
TTN_DEVICE_ID = "station-node"
TTN_TOPIC     = f"v3/{TTN_APP_ID}@ttn/devices/{TTN_DEVICE_ID}/up"

# ─────────────────────────────────────────────
#  GRAFANA CLOUD
# ─────────────────────────────────────────────
GRAFANA_URL   = "https://prometheus-prod-40-prod-sa-east-1.grafana.net/api/prom/push"
GRAFANA_USER  = "<GRAFANA_USER>"
GRAFANA_TOKEN = "<GRAFANA_TOKEN>"
STATION_ID    = "IQUITO132"
LOCATION      = "Quito-EC"

# ─────────────────────────────────────────────
#  LOGGING
# ─────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)
log = logging.getLogger(__name__)

# ─────────────────────────────────────────────
#  PROMETHEUS REMOTE_WRITE (SNPPY + PROTOBUF)
# ─────────────────────────────────────────────

def encode_varint(n):
    buf = []
    while True:
        towrite = n & 0x7f
        n >>= 7
        if n:
            buf.append(towrite | 0x80)
        else:
            buf.append(towrite)
            break
    return bytes(buf)

def encode_label(name, value):
    n = name.encode()
    v = value.encode()
    return (b'\x0a' + bytes([len(n)]) + n +
            b'\x12' + bytes([len(v)]) + v)

def encode_sample(val, timestamp):
    return (b'\x09' + struct.pack('<d', float(val)) +
            b'\x10' + encode_varint(timestamp))

def encode_timeseries(labels_dict, value, ts):
    labels_bytes = b''
    for k, v in labels_dict.items():
        lb = encode_label(k, v)
        labels_bytes += b'\x0a' + bytes([len(lb)]) + lb
    sample = encode_sample(value, ts)
    sample_field = b'\x12' + bytes([len(sample)]) + sample
    ts_bytes = labels_bytes + sample_field
    return b'\x0a' + bytes([len(ts_bytes)]) + ts_bytes

def send_to_grafana(metrics: dict):
    ts_ms = int(time.time() * 1000)
    write_request = b''

    for name, value in metrics.items():
        if value is None:
            continue
        labels = {
            "__name__": name,
            "station":  STATION_ID,
            "location": LOCATION
        }
        write_request += encode_timeseries(labels, value, ts_ms)

    compressed = snappy.compress(write_request)

    try:
        r = requests.post(
            GRAFANA_URL,
            headers={
                "Content-Encoding": "snappy",
                "Content-Type": "application/x-protobuf",
                "X-Prometheus-Remote-Write-Version": "0.1.0"
            },
            auth=(GRAFANA_USER, GRAFANA_TOKEN),
            data=compressed,
            timeout=10
        )
        if r.status_code in (200, 204):
            log.info("📊 Grafana → OK")
        else:
            log.error(f"Grafana → {r.status_code}: {r.text}")
    except Exception as e:
        log.error(f"Grafana error: {e}")

# ─────────────────────────────────────────────
#  PROCESAR MENSAJE TTN
# ─────────────────────────────────────────────

def process_uplink(payload: dict):
    try:
        uplink = payload.get("uplink_message", {})
        decoded = uplink.get("decoded_payload", {})

        if not decoded:
            log.warning("Sin decoded_payload en el mensaje")
            return

        # Extraemos RSSI y SNR del primer elemento de rx_metadata (suele ser suficiente)
        rx_meta = uplink.get("rx_metadata", [])
        rssi = rx_meta[0].get("rssi", None) if rx_meta else None
        snr  = rx_meta[0].get("snr",  None) if rx_meta else None

        # Mapeo de campos del payload a nombres de métricas en Prometheus
        metrics = {
            # Meteorología
            "weather_temperature_celsius":  decoded.get("temperature_out"),
            "weather_humidity_percent":     decoded.get("humidity"),
            "weather_pressure_hpa":         decoded.get("pressure_rel"),
            "weather_wind_speed_ms":        decoded.get("wind_speed"),
            "weather_wind_gust_ms":         decoded.get("wind_gust"),
            "weather_wind_direction_deg":   decoded.get("wind_direction"),
            "weather_rain_rate_mmh":        decoded.get("rain_rate"),
            "weather_rain_day_mm":          decoded.get("rain_day"),
            "weather_uv_index":             decoded.get("uv_index"),
            "weather_solar_radiation_wm2":  decoded.get("solar_radiation"),
            "weather_dew_point_celsius":    decoded.get("dew_point"),
            # Calidad del aire
            "weather_pm1_0_ugm3":           decoded.get("pm1_0"),
            "weather_pm2_5_ugm3":           decoded.get("pm2_5"),
            "weather_pm10_ugm3":            decoded.get("pm10"),
            "weather_co2_ppm":              decoded.get("co2_ppm"),
            # Señal LoRa (útil para monitorear calidad de enlace)
            "weather_rssi_dbm":             rssi,
            "weather_snr_db":               snr,
        }

        # Log estructurado con los valores principales para depuración
        log.info("─────────────────────────────────────")
        log.info(f"Temp: {decoded.get('temperature_out')}°C | "
                 f"Hum: {decoded.get('humidity')}% | "
                 f"Pres: {decoded.get('pressure_rel')} hPa")
        log.info(f"Viento: {decoded.get('wind_speed')} m/s | "
                 f"Rafaga: {decoded.get('wind_gust')} m/s | "
                 f"Dir: {decoded.get('wind_direction')}° {decoded.get('wind_cardinal','')}")
        log.info(f"Lluvia: {decoded.get('rain_rate')} mm/h | "
                 f"Dia: {decoded.get('rain_day')} mm")
        log.info(f"UV: {decoded.get('uv_index')} | "
                 f"Solar: {decoded.get('solar_radiation')} W/m²")
        log.info(f"PM1.0: {decoded.get('pm1_0')} | "
                 f"PM2.5: {decoded.get('pm2_5')} | "
                 f"PM10: {decoded.get('pm10')} µg/m³")
        log.info(f"CO2: {decoded.get('co2_ppm')} ppm [{decoded.get('co2_label','')}]")
        log.info(f"AQI: {decoded.get('aqi_label','')} | "
                 f"RSSI: {rssi} dBm | SNR: {snr} dB")
        log.info("─────────────────────────────────────")

        send_to_grafana(metrics)

    except Exception as e:
        log.error(f"Error procesando uplink: {e}")

# ─────────────────────────────────────────────
#  CLIENTE MQTT
# ─────────────────────────────────────────────

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info(f"✅ Conectado a TTN MQTT")
        client.subscribe(TTN_TOPIC, qos=0)
        log.info(f"📡 Suscrito a: {TTN_TOPIC}")
    else:
        log.error(f"❌ Error conexion MQTT: {rc}")

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        log.info(f"📨 Mensaje recibido de TTN")
        process_uplink(payload)
    except Exception as e:
        log.error(f"Error en mensaje MQTT: {e}")

def on_disconnect(client, userdata, rc, properties=None):
    log.warning(f"Desconectado de TTN (rc={rc}). Reconectando...")

# ─────────────────────────────────────────────
#  MAIN
# ─────────────────────────────────────────────

def main():
    log.info("=== TTN → Grafana Cloud ===")
    log.info(f"Broker  : {TTN_BROKER}:{TTN_PORT}")
    log.info(f"Topic   : {TTN_TOPIC}")
    log.info(f"Grafana : {GRAFANA_URL}")

    # Usamos client_id dinámico para evitar conflictos con múltiples instancias
    client = mqtt.Client(
        client_id=f"ttn-grafana-{int(time.time())}",
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2
    )
    client.username_pw_set(
        username=f"{TTN_APP_ID}@ttn",
        password=TTN_API_KEY
    )
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    client.connect(TTN_BROKER, TTN_PORT, keepalive=60)

    log.info("Esperando mensajes de TTN...")
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        log.info("Detenido por el usuario.")
        client.disconnect()

if __name__ == "__main__":
    main()