#include "common/geo_math.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace tcu::common {

namespace {
constexpr double kEarthRadiusMeters = 6371000.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

std::array<int, 3> parseSemVerParts(const std::string& v) noexcept {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::sscanf(v.c_str(), "%d.%d.%d", &major, &minor, &patch);
    return {major, minor, patch};
}
} // namespace

double haversineDistanceMeters(double lat1_deg, double lon1_deg,
                               double lat2_deg, double lon2_deg) noexcept {
    const double phi1 = lat1_deg * kDegToRad;
    const double phi2 = lat2_deg * kDegToRad;
    const double dphi = (lat2_deg - lat1_deg) * kDegToRad;
    const double dlambda = (lon2_deg - lon1_deg) * kDegToRad;

    const double sin_dphi = std::sin(dphi * 0.5);
    const double sin_dlambda = std::sin(dlambda * 0.5);
    const double a = (sin_dphi * sin_dphi) +
                     (std::cos(phi1) * std::cos(phi2) * sin_dlambda * sin_dlambda);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusMeters * c;
}

bool isPointInsidePolygon(
    double lat_deg,
    double lon_deg,
    const std::vector<std::pair<double, double>>& polygon_lat_lon) noexcept {
    const size_t n = polygon_lat_lon.size();
    if (n < 3) {
        return false;
    }
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = polygon_lat_lon[i].first;
        const double xi = polygon_lat_lon[i].second;
        const double yj = polygon_lat_lon[j].first;
        const double xj = polygon_lat_lon[j].second;

        const bool intersect = ((yi > lat_deg) != (yj > lat_deg)) &&
            (lon_deg < (xj - xi) * (lat_deg - yi) / ((yj - yi) + 1e-15) + xi);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

int compareSemVer(const std::string& v1, const std::string& v2) noexcept {
    const auto a = parseSemVerParts(v1);
    const auto b = parseSemVerParts(v2);
    for (size_t i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

} // namespace tcu::common

