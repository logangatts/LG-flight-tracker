// Great-circle / projection helpers
#pragma once

#include <math.h>
#include <stdint.h>

namespace geo {

inline constexpr float kEarthRadiusNm = 3440.065f;
inline constexpr float kDeg2Rad = 0.017453292519943295f;
inline constexpr float kRad2Deg = 57.29577951308232f;

// Distance in nautical miles (haversine).
float distanceNm(float lat1, float lon1, float lat2, float lon2);

// Initial bearing in degrees [0, 360).
float bearingDeg(float lat1, float lon1, float lat2, float lon2);

// Equirectangular projection of (lat, lon) around center -> x east / y north in nm.
// Good enough within radar range (< 250 nm).
void projectNm(float centerLat, float centerLon, float lat, float lon,
               float& xNm, float& yNm);

// Spherical interpolation along the great circle from (lat1,lon1) to (lat2,lon2).
// f in [0,1]. Outputs interpolated lat/lon.
void greatCirclePoint(float lat1, float lon1, float lat2, float lon2, float f,
                      float& latOut, float& lonOut);

// Web-mercator "world pixel" coordinates at zoom z (256 px tiles).
// Double precision: world coords at z12 exceed float's 24-bit mantissa.
void latLonToWorldPx(float lat, float lon, uint8_t z, double& wx, double& wy);

}  // namespace geo
