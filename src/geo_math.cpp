#include "geo_math.h"

namespace geo {

float distanceNm(float lat1, float lon1, float lat2, float lon2) {
  float p1 = lat1 * kDeg2Rad, p2 = lat2 * kDeg2Rad;
  float dp = (lat2 - lat1) * kDeg2Rad;
  float dl = (lon2 - lon1) * kDeg2Rad;
  float a = sinf(dp / 2) * sinf(dp / 2) +
            cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
  return kEarthRadiusNm * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}

float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float p1 = lat1 * kDeg2Rad, p2 = lat2 * kDeg2Rad;
  float dl = (lon2 - lon1) * kDeg2Rad;
  float y = sinf(dl) * cosf(p2);
  float x = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
  float b = atan2f(y, x) * kRad2Deg;
  return b < 0 ? b + 360.0f : b;
}

void projectNm(float centerLat, float centerLon, float lat, float lon,
               float& xNm, float& yNm) {
  yNm = (lat - centerLat) * 60.0f;
  xNm = (lon - centerLon) * 60.0f * cosf(centerLat * kDeg2Rad);
}

void greatCirclePoint(float lat1, float lon1, float lat2, float lon2, float f,
                      float& latOut, float& lonOut) {
  float p1 = lat1 * kDeg2Rad, l1 = lon1 * kDeg2Rad;
  float p2 = lat2 * kDeg2Rad, l2 = lon2 * kDeg2Rad;

  float d = 2 * asinf(sqrtf(sinf((p2 - p1) / 2) * sinf((p2 - p1) / 2) +
                            cosf(p1) * cosf(p2) *
                                sinf((l2 - l1) / 2) * sinf((l2 - l1) / 2)));
  if (d < 1e-6f) {
    latOut = lat1;
    lonOut = lon1;
    return;
  }
  float A = sinf((1 - f) * d) / sinf(d);
  float B = sinf(f * d) / sinf(d);
  float x = A * cosf(p1) * cosf(l1) + B * cosf(p2) * cosf(l2);
  float y = A * cosf(p1) * sinf(l1) + B * cosf(p2) * sinf(l2);
  float z = A * sinf(p1) + B * sinf(p2);
  latOut = atan2f(z, sqrtf(x * x + y * y)) * kRad2Deg;
  lonOut = atan2f(y, x) * kRad2Deg;
}

void latLonToWorldPx(float lat, float lon, uint8_t z, double& wx, double& wy) {
  double n = 256.0 * (double)(1UL << z);
  double latR = (double)lat * 0.017453292519943295;
  wx = ((double)lon + 180.0) / 360.0 * n;
  wy = (1.0 - asinh(tan(latR)) / M_PI) / 2.0 * n;
}

}  // namespace geo
