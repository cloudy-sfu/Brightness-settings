#include "brightness_curve.h"

float EvaluateBrightnessCurve(const std::vector<CurvePoint>& points, float lux) {
    if (points.empty()) return 0.0f;
    if (lux <= points.front().lux) return points.front().brightness;
    if (lux >= points.back().lux) return points.back().brightness;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (lux >= points[i].lux && lux <= points[i + 1].lux) {
            const float dx = points[i + 1].lux - points[i].lux;
            if (dx <= 1e-4f) return points[i].brightness;
            const float t = (lux - points[i].lux) / dx;
            return points[i].brightness +
                   t * (points[i + 1].brightness - points[i].brightness);
        }
    }

    return points.back().brightness;
}
