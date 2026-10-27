#include "sharp_memory_outline_atlas.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

struct point_t {
    double x;
    double y;
};

struct contour_t {
    std::vector<point_t> points;
    bool closed = false;
};

// Compact physical-pixel periods keep brightness visible on short UI strokes.
// In particular, a four-Cairo-pixel step marker is only twelve Sharp pixels
// long, so the former 16--80 pixel dashes made levels 1 through 6 look solid
// and nearly identical. Levels 1--7 now use the requested N-on/1-off center
// track. Levels 8--14 progressively add staggered side tracks; level 15 is a
// solid three-pixel contour.
static const sharp_memory_outline_style_t styles[16] = {
    {1, {0, 0, 0}, {0, 0, 0}},
    {2, {0, 1, 0}, {0, 0, 0}},
    {3, {0, 2, 0}, {0, 0, 0}},
    {4, {0, 3, 0}, {0, 0, 0}},
    {5, {0, 4, 0}, {0, 0, 0}},
    {6, {0, 5, 0}, {0, 0, 0}},
    {7, {0, 6, 0}, {0, 0, 0}},
    {8, {0, 7, 0}, {0, 0, 0}},
    {4, {1, 4, 0}, {0, 0, 2}},
    {4, {1, 4, 1}, {0, 0, 2}},
    {4, {2, 4, 1}, {0, 0, 2}},
    {4, {2, 4, 2}, {0, 0, 2}},
    {4, {3, 4, 2}, {0, 0, 2}},
    {4, {4, 4, 2}, {0, 0, 2}},
    {4, {4, 4, 3}, {0, 0, 2}},
    {4, {4, 4, 4}, {0, 0, 2}},
};

static point_t add(point_t a, point_t b) {
    return {a.x + b.x, a.y + b.y};
}

static point_t subtract(point_t a, point_t b) {
    return {a.x - b.x, a.y - b.y};
}

static point_t multiply(point_t point, double scalar) {
    return {point.x * scalar, point.y * scalar};
}

static double dot(point_t a, point_t b) {
    return a.x * b.x + a.y * b.y;
}

static double length(point_t point) {
    return std::hypot(point.x, point.y);
}

static bool same_point(point_t a, point_t b) {
    return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6;
}

static point_t segment_normal(point_t start, point_t end) {
    const point_t direction = subtract(end, start);
    const double segment_length = length(direction);
    if (segment_length < 1e-9)
        return {0.0, 0.0};
    return {-direction.y / segment_length, direction.x / segment_length};
}

static void finish_contour(std::vector<contour_t> &contours, contour_t &contour) {
    while (contour.points.size() > 1 &&
           same_point(contour.points.back(), contour.points[contour.points.size() - 2]))
        contour.points.pop_back();
    if (contour.closed && contour.points.size() > 1 &&
        same_point(contour.points.front(), contour.points.back()))
        contour.points.pop_back();
    if (contour.points.size() >= 2)
        contours.push_back(contour);
    contour = contour_t{};
}

static std::vector<contour_t> read_contours(cairo_t *path_context,
                                            const cairo_path_t *path) {
    std::vector<contour_t> contours;
    contour_t contour;

    for (int index = 0; index < path->num_data;) {
        const cairo_path_data_t *data = &path->data[index];
        switch (data->header.type) {
        case CAIRO_PATH_MOVE_TO: {
            finish_contour(contours, contour);
            double x = data[1].point.x;
            double y = data[1].point.y;
            cairo_user_to_device(path_context, &x, &y);
            contour.points.push_back({x, y});
            break;
        }
        case CAIRO_PATH_LINE_TO: {
            double x = data[1].point.x;
            double y = data[1].point.y;
            cairo_user_to_device(path_context, &x, &y);
            const point_t point = {x, y};
            if (contour.points.empty() || !same_point(contour.points.back(), point))
                contour.points.push_back(point);
            break;
        }
        case CAIRO_PATH_CLOSE_PATH:
            contour.closed = true;
            finish_contour(contours, contour);
            break;
        default:
            break;
        }
        index += data->header.length;
    }
    finish_contour(contours, contour);
    return contours;
}

static point_t offset_vertex(const contour_t &contour,
                             size_t index,
                             double offset,
                             double miter_limit) {
    const size_t count = contour.points.size();
    if (!contour.closed && index == 0)
        return add(contour.points[index],
                   multiply(segment_normal(contour.points[0], contour.points[1]), offset));
    if (!contour.closed && index + 1 == count)
        return add(contour.points[index],
                   multiply(segment_normal(contour.points[count - 2], contour.points[count - 1]),
                            offset));

    const size_t previous = index == 0 ? count - 1 : index - 1;
    const size_t next = index + 1 == count ? 0 : index + 1;
    const point_t previous_normal =
        segment_normal(contour.points[previous], contour.points[index]);
    const point_t next_normal = segment_normal(contour.points[index], contour.points[next]);
    const point_t sum = add(previous_normal, next_normal);
    const double sum_length = length(sum);
    if (sum_length < 1e-6)
        return add(contour.points[index], multiply(next_normal, offset));

    const point_t miter = multiply(sum, 1.0 / sum_length);
    const double denominator = dot(miter, next_normal);
    if (std::abs(denominator) < 1e-6)
        return add(contour.points[index], multiply(next_normal, offset));

    const double miter_distance = offset / denominator;
    const double maximum = std::abs(offset) * std::max(1.0, miter_limit);
    if (std::abs(miter_distance) > maximum)
        return add(contour.points[index], multiply(miter, std::copysign(maximum, offset)));
    return add(contour.points[index], multiply(miter, miter_distance));
}

static void append_offset_contour(cairo_t *context,
                                  const contour_t &contour,
                                  double offset,
                                  double miter_limit) {
    const point_t first = offset_vertex(contour, 0, offset, miter_limit);
    cairo_move_to(context, first.x, first.y);
    for (size_t index = 1; index < contour.points.size(); ++index) {
        const point_t point = offset_vertex(contour, index, offset, miter_limit);
        cairo_line_to(context, point.x, point.y);
    }
    if (contour.closed)
        cairo_close_path(context);
}

} // namespace

sharp_memory_outline_style_t sharp_memory_outline_style(uint8_t level) {
    return styles[level > 15 ? 15 : level];
}

void sharp_memory_outline_render(cairo_t *path_context,
                                 const cairo_path_t *flat_path,
                                 cairo_surface_t *target,
                                 uint8_t level,
                                 double track_width_px) {
    if (path_context == nullptr || flat_path == nullptr || target == nullptr || level == 0 ||
        track_width_px <= 0.0)
        return;

    const std::vector<contour_t> contours = read_contours(path_context, flat_path);
    if (contours.empty())
        return;

    const sharp_memory_outline_style_t style = sharp_memory_outline_style(level);
    cairo_t *context = cairo_create(target);
    cairo_set_antialias(context, CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width(context, track_width_px);
    cairo_set_line_cap(context, cairo_get_line_cap(path_context));
    cairo_set_line_join(context, cairo_get_line_join(path_context));
    cairo_set_miter_limit(context, cairo_get_miter_limit(path_context));

    const double offsets[SHARP_MEMORY_OUTLINE_TRACK_COUNT] = {
        -track_width_px,
        0.0,
        track_width_px,
    };

    for (int track = 0; track < SHARP_MEMORY_OUTLINE_TRACK_COUNT; ++track) {
        const uint8_t dash = style.dash_px[track];
        if (dash == 0)
            continue;

        cairo_new_path(context);
        for (const contour_t &contour : contours)
            append_offset_contour(
                context, contour, offsets[track], cairo_get_miter_limit(path_context));

        if (dash >= style.period_px) {
            cairo_set_dash(context, nullptr, 0, 0.0);
        } else {
            const double dashes[2] = {
                static_cast<double>(dash),
                static_cast<double>(style.period_px - dash),
            };
            cairo_set_dash(context, dashes, 2, style.phase_px[track]);
        }
        cairo_stroke(context);
    }

    cairo_destroy(context);
}
