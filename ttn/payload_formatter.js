function decodeUplink(input) {
  var b = input.bytes;
  function readInt16(i) {
    var v = (b[i] << 8) | b[i+1];
    return v > 32767 ? v - 65536 : v;
  }
  function readUInt16(i) {
    return (b[i] << 8) | b[i+1];
  }

  var tempOut   = readInt16(0)  / 10;
  var humidity  = readUInt16(2);
  var pressure  = readUInt16(4) / 10;
  var windSpd   = readUInt16(6) / 10;
  var windGust  = readUInt16(8) / 10;
  var windDir   = readUInt16(10);
  var rainRate  = readUInt16(12) / 10;
  var rainDay   = readUInt16(14) / 10;
  var uv        = b[16];
  var solar     = b[17] * 4;
  var pm1_0     = readUInt16(18) / 10;
  var pm2_5     = readUInt16(20) / 10;
  var pm10      = readUInt16(22) / 10;
  var co2       = readUInt16(24);

  // Punto de rocío (Magnus)
  var a = 17.27, bv = 237.7;
  var alpha = ((a * tempOut) / (bv + tempOut)) +
              Math.log(humidity / 100.0);
  var dewPoint = Math.round((bv * alpha) / (a - alpha) * 10) / 10;

  // Dirección cardinal
  var dirs = ["N","NNE","NE","ENE","E","ESE","SE","SSE",
              "S","SSO","SO","OSO","O","ONO","NO","NNO"];
  var cardinal = dirs[Math.round(windDir / 22.5) % 16];

  return {
    data: {
      temperature_out:  tempOut,
      humidity:         humidity,
      pressure_rel:     pressure,
      wind_speed:       windSpd,
      wind_gust:        windGust,
      wind_direction:   windDir,
      wind_cardinal:    cardinal,
      rain_rate:        rainRate,
      rain_day:         rainDay,
      uv_index:         uv,
      solar_radiation:  solar,
      dew_point:        dewPoint,
      pm1_0:            pm1_0,
      pm2_5:            pm2_5,
      pm10:             pm10,
      co2_ppm:          co2
    },
    warnings: []
  };
}