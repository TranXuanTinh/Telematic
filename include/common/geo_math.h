#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tcu::common {

/// Compute great-circle Haversine distance in meters between two WGS84 coordinates.
double haversineDistanceMeters(double lat1_deg, double lon1_deg,
                               double lat2_deg, double lon2_deg) noexcept;

/// Ray-casting Point-in-Polygon check for custom polygonal SVT geofences.
bool isPointInsidePolygon(
    double lat_deg,
    double lon_deg,
    const std::vector<std::pair<double, double>>& polygon_lat_lon) noexcept;

/// Compare semantic version strings "major.minor.patch" (returns <0, 0, >0).
/// Used by OTA anti-rollback checks.
int compareSemVer(const std::string& v1, const std::string& v2) noexcept;

} // namespace tcu::common

