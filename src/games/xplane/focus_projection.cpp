#include "focus_projection.h"

#include <cmath>

namespace xplane {
namespace {

// Element (row, column) of the matrix in either layout.
int at(int row, int col, bool columnMajor) { return columnMajor ? col * 4 + row : row * 4 + col; }

}  // namespace

bool projection_layout(const double (&m)[16], bool& columnMajor) {
    // The other of the two holds the depth term (X-Plane's reversed-Z: the near distance), so only the -1 decides.
    if (m[11] == -1.0 && m[14] != -1.0) {
        columnMajor = true;
        return true;
    }
    if (m[14] == -1.0 && m[11] != -1.0) {
        columnMajor = false;
        return true;
    }
    return false;
}

bool projection_tangents(const double (&m)[16], double (&tan)[4]) {
    bool cm = false;
    if (!projection_layout(m, cm))
        return false;
    for (int row = 0; row < 2; ++row) {
        const double a = m[at(row, row, cm)], b = m[at(row, 2, cm)];
        if (a == 0.0 || !std::isfinite(a) || !std::isfinite(b))
            return false;
        const double t1 = (-1.0 + b) / a, t2 = (1.0 + b) / a;
        tan[row * 2] = t1 < t2 ? t1 : t2;
        tan[row * 2 + 1] = t1 < t2 ? t2 : t1;
    }
    return true;
}

bool focus_projection(const double (&eye)[16], double fraction, double fractionY, double cx, double cy, double (&out)[16]) {
    for (int i = 0; i < 16; ++i)
        out[i] = eye[i];
    bool cm = false;
    double tan[4];
    if (!projection_layout(eye, cm) || !projection_tangents(eye, tan) || !(fraction > 0.0) || !(fractionY > 0.0))
        return false;
    const double half[2] = {0.5 * fraction * (tan[1] - tan[0]), 0.5 * fractionY * (tan[3] - tan[2])};
    const double centre[2] = {cx, cy};
    for (int row = 0; row < 2; ++row) {
        const double a = eye[at(row, row, cm)];
        const double s = a < 0.0 ? -1.0 : 1.0;
        out[at(row, row, cm)] = s / half[row];
        out[at(row, 2, cm)] = s * centre[row] / half[row];
        // the row's other entries stay as the eye's (0 for a projection without shear)
    }
    return true;
}

}  // namespace xplane
