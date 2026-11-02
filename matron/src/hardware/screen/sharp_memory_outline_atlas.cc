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

// A compact eight-physical-pixel period keeps brightness visible on short UI
// strokes. Levels 1--6 progressively fill the center track, level 7 makes it
// solid, and levels 8--14 add equal upper and lower tracks. Keeping both side
// tracks identical makes the apparent stroke position independent of path
// direction. Level 15 is a solid three-pixel contour.
static const sharp_memory_outline_style_t styles[16] = {
    {1, {0, 0, 0}, {0, 0, 0}},
    {8, {0, 1, 0}, {0, 0, 0}},
    {8, {0, 2, 0}, {0, 0, 0}},
    {8, {0, 3, 0}, {0, 0, 0}},
    {8, {0, 4, 0}, {0, 0, 0}},
    {8, {0, 5, 0}, {0, 0, 0}},
    {8, {0, 6, 0}, {0, 0, 0}},
    {8, {0, 8, 0}, {0, 0, 0}},
    {8, {1, 8, 1}, {0, 0, 0}},
    {8, {2, 8, 2}, {0, 0, 0}},
    {8, {3, 8, 3}, {0, 0, 0}},
    {8, {4, 8, 4}, {0, 0, 0}},
    {8, {5, 8, 5}, {0, 0, 0}},
    {8, {6, 8, 6}, {0, 0, 0}},
    {8, {7, 8, 7}, {0, 0, 0}},
    {8, {8, 8, 8}, {0, 0, 0}},
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

static bool integer_coordinate(double value) {
    return std::abs(value - std::round(value)) < 1e-6;
}

static bool odd_integer_width(double width) {
    const double rounded = std::round(width);
    return std::abs(width - rounded) < 1e-6 &&
           (static_cast<int>(rounded) & 1) != 0;
}

static void align_contour_to_cairo_pixels(contour_t &contour, double track_width_px) {
    // With antialiasing disabled, Cairo assigns an odd-width stroke centered
    // on an integer coordinate to the pixel immediately above/left. At 3x,
    // the geometric outline otherwise lands one physical pixel below/right
    // of the expanded canonical coverage. Only shift normal to axis-aligned
    // segments: their endpoints along the tangent are already correct.
    if (!odd_integer_width(track_width_px) || contour.points.size() < 2)
        return;

    std::vector<bool> align_x(contour.points.size(), false);
    std::vector<bool> align_y(contour.points.size(), false);
    const size_t segment_count = contour.closed ? contour.points.size()
                                                : contour.points.size() - 1;
    for (size_t index = 0; index < segment_count; ++index) {
        const size_t next = (index + 1) % contour.points.size();
        const point_t start = contour.points[index];
        const point_t end = contour.points[next];
        if (std::abs(start.x - end.x) < 1e-6 && integer_coordinate(start.x)) {
            align_x[index] = true;
            align_x[next] = true;
        }
        if (std::abs(start.y - end.y) < 1e-6 && integer_coordinate(start.y)) {
            align_y[index] = true;
            align_y[next] = true;
        }
    }

    for (size_t index = 0; index < contour.points.size(); ++index) {
        if (align_x[index])
            contour.points[index].x -= 1.0;
        if (align_y[index])
            contour.points[index].y -= 1.0;
    }
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

static std::vector<point_t> offset_contour_points(const contour_t &contour,
                                                  double offset,
                                                  double miter_limit) {
    std::vector<point_t> points;
    points.reserve(contour.points.size());
    for (size_t index = 0; index < contour.points.size(); ++index)
        points.push_back(offset_vertex(contour, index, offset, miter_limit));
    return points;
}

static double contour_length(const std::vector<point_t> &points, bool closed) {
    if (points.size() < 2)
        return 0.0;
    double total = 0.0;
    const size_t segments = closed ? points.size() : points.size() - 1;
    for (size_t index = 0; index < segments; ++index)
        total += length(subtract(points[(index + 1) % points.size()], points[index]));
    return total;
}

static void append_contour(cairo_t *context,
                           const std::vector<point_t> &points,
                           bool closed) {
    if (points.size() < 2)
        return;
    cairo_move_to(context, points[0].x, points[0].y);
    for (size_t index = 1; index < points.size(); ++index)
        cairo_line_to(context, points[index].x, points[index].y);
    if (closed)
        cairo_close_path(context);
}

static void set_fitted_open_dash(cairo_t *context,
                                 double total,
                                 double dash,
                                 double period) {
    if (total < 1e-9 || dash <= 0.0 || period <= 0.0)
        return;
    if (total <= dash + 1e-9) {
        cairo_set_dash(context, nullptr, 0, 0.0);
        return;
    }

    const double gap = period - dash;
    // Choose the closest integer number of whole dash blocks, then scale dash
    // and gap together just enough for the sequence to start and end with
    // equal complete dashes. This preserves mean coverage on short strokes
    // instead of dropping a high-level dash that does not fit.
    const size_t count = std::max<size_t>(
        static_cast<size_t>(std::floor((total + gap) / period + 0.5)), 1);
    const double nominal_span = count * dash + (count - 1) * gap;
    const double scale = total / nominal_span;
    const double fitted_dash = dash * scale;
    const double fitted_gap = gap * scale;
    const double dashes[2] = {fitted_dash, fitted_gap};
    cairo_set_dash(context, dashes, 2, 0.0);
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

    std::vector<contour_t> contours = read_contours(path_context, flat_path);
    if (contours.empty())
        return;

    for (contour_t &contour : contours)
        align_contour_to_cairo_pixels(contour, track_width_px);

    const sharp_memory_outline_style_t style = sharp_memory_outline_style(level);
    cairo_t *context = cairo_create(target);
    cairo_set_antialias(context, CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    // Alpha is the mask consumed by the Sharp backend. Preserve the source
    // norns level in RGB as diagnostic metadata so sharp-stroke.png exposes
    // path/level leaks without changing the rendered display.
    const double encoded_level = static_cast<double>(level) / 15.0;
    cairo_set_source_rgba(
        context, encoded_level, encoded_level, encoded_level, 1.0);
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

        for (const contour_t &contour : contours) {
            const std::vector<point_t> points = offset_contour_points(
                contour, offsets[track], cairo_get_miter_limit(path_context));
            const double total = contour_length(points, contour.closed);
            if (total < 1e-9)
                continue;

            cairo_new_path(context);
            if (dash >= style.period_px) {
                cairo_set_dash(context, nullptr, 0, 0.0);
                append_contour(context, points, contour.closed);
            } else if (!contour.closed) {
                set_fitted_open_dash(context,
                                     total,
                                     static_cast<double>(dash),
                                     static_cast<double>(style.period_px));
                append_contour(context, points, false);
            } else {
                // A closed contour has no meaningful center. Fit an integer
                // number of periods around its perimeter so the dash pattern
                // reaches the seam in exactly the same state in which it left.
                const double nominal_period = static_cast<double>(style.period_px);
                const double cycles = std::max(1.0, std::round(total / nominal_period));
                const double period = total / cycles;
                const double scaled_dash = period * dash / nominal_period;
                const double dashes[2] = {scaled_dash, period - scaled_dash};
                const double phase = style.phase_px[track] * period / nominal_period;
                cairo_set_dash(context, dashes, 2, phase);
                append_contour(context, points, true);
            }
            cairo_stroke(context);
        }
    }

    cairo_destroy(context);
}
