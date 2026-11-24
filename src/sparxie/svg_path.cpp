#include "src/sparxie/svg_path.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace sparxie {
  namespace {
    struct Point {
      double x = 0;
      double y = 0;
    };
    using Polygon = std::vector<Point>;

    // A tokenizer over path data: commands, numbers (compact forms like "-.5.5" and arc flags written without separators included).
    struct Reader {
      std::string_view text;
      std::size_t at = 0;
      bool bad = false;

      void
      skip() {
        while (at < text.size() && (text[at] == ' ' || text[at] == ',' || text[at] == '\n' || text[at] == '\t' || text[at] == '\r')) ++at;
      }
      bool
      more() {
        skip();
        return at < text.size();
      }
      bool
      at_number() {
        skip();
        if (at >= text.size()) return false;
        const char c = text[at];
        return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
      }
      double
      number() {
        skip();
        const std::size_t start = at;
        if (at < text.size() && (text[at] == '-' || text[at] == '+')) ++at;
        bool digits = false;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
          ++at;
          digits = true;
        }
        if (at < text.size() && text[at] == '.') {
          ++at;
          while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            ++at;
            digits = true;
          }
        }
        if (!digits) {
          bad = true;
          return 0;
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
          std::size_t probe = at + 1;
          if (probe < text.size() && (text[probe] == '-' || text[probe] == '+')) ++probe;
          if (probe < text.size() && text[probe] >= '0' && text[probe] <= '9') {
            while (probe < text.size() && text[probe] >= '0' && text[probe] <= '9') ++probe;
            at = probe;
          }
        }
        return std::strtod(std::string(text.substr(start, at - start)).c_str(), nullptr);
      }
      // an arc flag: one character, 0 or 1
      bool
      flag() {
        skip();
        if (at >= text.size() || (text[at] != '0' && text[at] != '1')) {
          bad = true;
          return false;
        }
        return text[at++] == '1';
      }
    };

    void
    add_cubic(Polygon &poly, Point p0, Point p1, Point p2, Point p3) {
      constexpr int steps = 14;
      for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps, u = 1 - t;
        poly.push_back({ u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x, u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y });
      }
    }

    // An SVG endpoint arc (the implementation notes of the SVG specification, F.6) as a polyline.
    void
    add_arc(Polygon &poly, Point from, double rx, double ry, double rotation_degrees, bool large, bool sweep, Point to) {
      if (from.x == to.x && from.y == to.y) return;
      rx = std::fabs(rx);
      ry = std::fabs(ry);
      if (rx == 0 || ry == 0) {
        poly.push_back(to);
        return;
      }
      const double pi = 3.14159265358979323846;
      const double phi = rotation_degrees * pi / 180.0, cp = std::cos(phi), sp = std::sin(phi);
      const double dx = (from.x - to.x) / 2, dy = (from.y - to.y) / 2;
      const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
      const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
      if (lambda > 1) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
      }
      const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
      const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
      double coef = den == 0 ? 0 : std::sqrt(std::max(0.0, num / den));
      if (large == sweep) coef = -coef;
      const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
      const double cx = cp * cxp - sp * cyp + (from.x + to.x) / 2, cy = sp * cxp + cp * cyp + (from.y + to.y) / 2;
      const auto angle = [](double ux, double uy, double vx, double vy) {
        const double dot = ux * vx + uy * vy, len = std::sqrt(ux * ux + uy * uy) * std::sqrt(vx * vx + vy * vy);
        double a = std::acos(std::clamp(len == 0 ? 1.0 : dot / len, -1.0, 1.0));
        if (ux * vy - uy * vx < 0) a = -a;
        return a;
      };
      const double theta = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
      double delta = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
      if (!sweep && delta > 0) delta -= 2 * pi;
      if (sweep && delta < 0) delta += 2 * pi;
      const int steps = std::max(4, static_cast<int>(std::ceil(std::fabs(delta) / (pi / 24))));
      for (int i = 1; i <= steps; ++i) {
        const double a = theta + delta * i / steps;
        const double ex = rx * std::cos(a), ey = ry * std::sin(a);
        poly.push_back({ cp * ex - sp * ey + cx, sp * ex + cp * ey + cy });
      }
    }

    bool
    flatten(std::string_view d, std::vector<Polygon> &out) {
      Reader in { d };
      Point cur, start, last_ctrl;
      char command = 0, previous = 0;
      Polygon poly;
      const auto close = [&] {
        if (poly.size() > 1) out.push_back(poly);
        poly.clear();
      };
      while (in.more()) {
        if ((d[in.at] >= 'A' && d[in.at] <= 'Z') || (d[in.at] >= 'a' && d[in.at] <= 'z')) command = d[in.at++];
        else if (!command)
          return false;
        else if (command == 'M')
          command = 'L';
        else if (command == 'm')
          command = 'l';
        const bool rel = command >= 'a';
        const char kind = static_cast<char>(rel ? command - 32 : command);
        const double ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
        switch (kind) {
          case 'Z':
            cur = start;
            close();
            poly.push_back(cur);
            previous = kind;
            continue;
          case 'M': {
            const double x = in.number() + ox, y = in.number() + oy;
            close();
            cur = start = { x, y };
            poly.push_back(cur);
            break;
          }
          case 'L': {
            const double x = in.number() + ox, y = in.number() + oy;
            cur = { x, y };
            poly.push_back(cur);
            break;
          }
          case 'H':
            cur.x = in.number() + ox;
            poly.push_back(cur);
            break;
          case 'V':
            cur.y = in.number() + oy;
            poly.push_back(cur);
            break;
          case 'C': {
            const Point p1 { in.number() + ox, in.number() + oy }, p2 { in.number() + ox, in.number() + oy }, p3 { in.number() + ox, in.number() + oy };
            add_cubic(poly, cur, p1, p2, p3);
            last_ctrl = p2;
            cur = p3;
            break;
          }
          case 'S': {
            const Point p1 = previous == 'C' || previous == 'S' ? Point { 2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y } : cur;
            const Point p2 { in.number() + ox, in.number() + oy }, p3 { in.number() + ox, in.number() + oy };
            add_cubic(poly, cur, p1, p2, p3);
            last_ctrl = p2;
            cur = p3;
            break;
          }
          case 'Q': {
            const Point q { in.number() + ox, in.number() + oy }, p3 { in.number() + ox, in.number() + oy };
            add_cubic(poly, cur, { cur.x + 2.0 / 3 * (q.x - cur.x), cur.y + 2.0 / 3 * (q.y - cur.y) }, { p3.x + 2.0 / 3 * (q.x - p3.x), p3.y + 2.0 / 3 * (q.y - p3.y) }, p3);
            cur = p3;
            break;
          }
          case 'A': {
            const double rx = in.number(), ry = in.number(), rotation = in.number();
            const bool large = in.flag(), sweep = in.flag();
            const Point to { in.number() + ox, in.number() + oy };
            add_arc(poly, cur, rx, ry, rotation, large, sweep, to);
            cur = to;
            break;
          }
          default:
            return false;
        }
        if (in.bad) return false;
        previous = kind;
      }
      close();
      return true;
    }
  }  // namespace

  Rgba
  rasterize_svg_path(std::string_view d, double view_w, double view_h, int out_w) {
    Rgba out;
    if (view_w <= 0 || view_h <= 0 || out_w <= 0 || out_w > 4096) return out;
    const double scale = out_w / view_w;
    const int out_h = std::max(1, static_cast<int>(std::lround(view_h * scale)));
    std::vector<Polygon> polygons;
    if (!flatten(d, polygons) || polygons.empty()) return out;
    constexpr int sub = 4;
    std::vector<float> coverage(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h), 0.f);
    std::vector<double> crossings;
    for (int row = 0; row < out_h * sub; ++row) {
      const double y = (row + 0.5) / sub;
      crossings.clear();
      for (const Polygon &poly : polygons)
        for (std::size_t i = 0; i < poly.size(); ++i) {
          const Point a = poly[i], b = poly[(i + 1) % poly.size()];
          const double ay = a.y * scale, by = b.y * scale;
          if ((ay <= y && by > y) || (by <= y && ay > y)) crossings.push_back(a.x * scale + (y - ay) * (b.x * scale - a.x * scale) / (by - ay));
        }
      std::sort(crossings.begin(), crossings.end());
      for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
        const double x0 = std::clamp(crossings[i], 0.0, static_cast<double>(out_w)), x1 = std::clamp(crossings[i + 1], 0.0, static_cast<double>(out_w));
        if (x1 <= x0) continue;
        const int first = static_cast<int>(std::floor(x0)), last = std::min(out_w - 1, static_cast<int>(std::floor(x1)));
        for (int x = first; x <= last; ++x) {
          const double overlap = std::min<double>(x + 1, x1) - std::max<double>(x, x0);
          coverage[static_cast<std::size_t>(row / sub) * static_cast<std::size_t>(out_w) + static_cast<std::size_t>(x)] += static_cast<float>(overlap / sub);
        }
      }
    }
    out.w = out_w;
    out.h = out_h;
    out.px.assign(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4, 255);
    for (std::size_t i = 0; i < coverage.size(); ++i) out.px[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(coverage[i], 0.f, 1.f) * 255.f));
    return out;
  }

  std::string
  svg_path_of_page(const std::string &page, std::string_view id) {
    const std::string marker = "id=\"" + std::string(id) + "\"";
    const auto at = page.find(marker);
    if (at == std::string::npos) return {};
    const auto open = page.rfind("<path", at);
    const auto close = page.find('>', at);
    if (open == std::string::npos || close == std::string::npos || close - open > 400000) return {};
    const std::string tag = page.substr(open, close - open);
    const auto d = tag.find(" d=\"");
    if (d == std::string::npos) return {};
    const auto end = tag.find('"', d + 4);
    return end == std::string::npos ? std::string() : tag.substr(d + 4, end - d - 4);
  }
}  // namespace sparxie
