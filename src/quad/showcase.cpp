// The showcase's A/B table (showcase.h): each side's sums and the rows the overlay's card shows.
#include "showcase.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace simqvfr {

namespace {

std::wstring fixed(double v, int decimals) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

std::wstring signed_fixed(double v, int decimals) {
    // A difference that rounds to zero shows as 0, without a sign.
    if (std::fabs(v) < 0.5 * std::pow(10.0, -decimals))
        return fixed(0, decimals);
    return (v > 0 ? L"+" : L"−") + fixed(std::fabs(v), decimals);
}

std::wstring percent_of(double qvfr, double stock) {
    if (stock <= 0)
        return {};
    const double p = (qvfr / stock - 1) * 100;
    return L" (" + signed_fixed(p, 0) + L"%)";
}

// Higher is better (fps, PPD) or lower is (ms, pixels): Good when QVFR is better by more than a hair, Warn when worse.
OsdTone tone_of(double qvfr, double stock, bool higherBetter) {
    if (stock <= 0 || qvfr <= 0)
        return OsdTone::Neutral;
    const double r = qvfr / stock - 1;
    if (std::fabs(r) < 0.01)
        return OsdTone::Neutral;
    return (r > 0) == higherBetter ? OsdTone::Good : OsdTone::Warn;
}

const std::wstring kDash = L"—";

}  // namespace

AbStats ab_stats(const AbSide& side) {
    AbStats s;
    if (!side.frames || side.frameMsSum <= 0)
        return s;
    s.valid = true;
    s.seconds = side.frameMsSum / 1000;
    s.fps = side.frames * 1000.0 / side.frameMsSum;
    // The 1% low as the overlay's card has it: 1000 / the 99th percentile frame time (the bin's upper edge).
    const uint64_t rank = side.frames - side.frames / 100;  // frames at or below the 99th percentile
    uint64_t seen = 0;
    for (int b = 0; b < kAbBins; ++b) {
        seen += side.bins[b];
        if (seen >= rank) {
            s.low1Fps = 1000.0 / ((b + 1) * kAbBinMs);
            break;
        }
    }
    if (side.cpuFrames)
        s.cpuMs = side.cpuMsSum / side.cpuFrames;
    if (side.gpuFrames)
        s.gpuMs = side.gpuMsSum / side.gpuFrames;
    return s;
}

void AbTable::reset() {
    sides_ = {};
    last_ = -1;
    settleUntil_ = 0;
}

void AbTable::add(bool stock, const std::vector<double>& frameMs, double cpuMs, double gpuMs, int64_t nowMs,
                  uint64_t sceneFrames) {
    if (frameMs.empty() || static_cast<double>(sceneFrames) < kAbSceneShare * static_cast<double>(frameMs.size())) {
        last_ = -1;  // a menu, loading or pause: the next window on the track settles first
        return;
    }
    const int side = stock ? 0 : 1;
    if (side != last_) {  // the first window on this side: its frames may straddle the switch
        last_ = side;
        settleUntil_ = nowMs + kAbSettleMs;
        return;
    }
    if (nowMs < settleUntil_ || frameMs.empty())
        return;
    AbSide& s = sides_[side];
    for (const double ms : frameMs) {
        if (!(ms > 0))
            continue;
        ++s.frames;
        s.frameMsSum += ms;
        const int bin = std::min(kAbBins - 1, static_cast<int>(ms / kAbBinMs));
        ++s.bins[bin];
    }
    const uint64_t n = frameMs.size();
    if (cpuMs >= 0) {
        s.cpuMsSum += cpuMs * n;
        s.cpuFrames += n;
    }
    if (gpuMs >= 0) {
        s.gpuMsSum += gpuMs * n;
        s.gpuFrames += n;
    }
}

std::vector<AbLine> ab_lines(const AbStats& stock, const AbStats& qvfr, const AbPixels& pixels) {
    std::vector<AbLine> lines;
    // A measured row: each side's value (a dash until it has frames) and QVFR's difference with its percentage.
    const auto measured = [&](const wchar_t* label, double s, double q, bool sValid, bool qValid, int decimals,
                              const wchar_t* unit, bool higherBetter) {
        AbLine line{label};
        sValid = sValid && s > 0;
        qValid = qValid && q > 0;
        line.stock = sValid ? fixed(s, decimals) + unit : kDash;
        line.qvfr = qValid ? fixed(q, decimals) + unit : kDash;
        if (sValid && qValid) {
            line.delta = signed_fixed(q - s, decimals) + unit + percent_of(q, s);
            line.tone = tone_of(q, s, higherBetter);
        } else {
            line.delta = kDash;
        }
        lines.push_back(line);
    };
    measured(L"Average FPS", stock.fps, qvfr.fps, stock.valid, qvfr.valid, 1, L"", true);
    measured(L"1% low FPS", stock.low1Fps, qvfr.low1Fps, stock.valid, qvfr.valid, 1, L"", true);
    measured(L"CPU frame time", stock.cpuMs, qvfr.cpuMs, stock.valid, qvfr.valid, 2, L" ms", false);
    measured(L"GPU frame time", stock.gpuMs, qvfr.gpuMs, stock.valid, qvfr.valid, 2, L" ms", false);
    // Pixels, millions for both eyes: the difference as a percentage alone.
    {
        AbLine line{L"Pixels rendered"};
        const bool known = pixels.stockPixels > 0 && pixels.qvfrPixels > 0;
        line.stock = pixels.stockPixels > 0 ? fixed(pixels.stockPixels / 1e6, 1) + L" MP" : kDash;
        line.qvfr = pixels.qvfrPixels > 0 ? fixed(pixels.qvfrPixels / 1e6, 1) + L" MP" : kDash;
        line.delta = known ? signed_fixed((pixels.qvfrPixels / pixels.stockPixels - 1) * 100, 0) + L"%" : kDash;
        line.tone = known ? tone_of(pixels.qvfrPixels, pixels.stockPixels, false) : OsdTone::Neutral;
        lines.push_back(line);
    }
    const auto ppd = [&](const wchar_t* label, double s, double q, bool trade) {
        AbLine line{label};
        line.stock = s > 0 ? fixed(s, 0) + L" PPD" : kDash;
        line.qvfr = q > 0 ? fixed(q, 0) + L" PPD" : kDash;
        if (s > 0 && q > 0) {
            line.delta = signed_fixed(q - s, 0) + percent_of(q, s);
            line.tone = trade ? OsdTone::Neutral : tone_of(q, s, true);
        } else {
            line.delta = kDash;
        }
        lines.push_back(line);
    };
    ppd(L"Sharpness, centre", pixels.stockPpd, pixels.focusPpd, false);
    ppd(L"Sharpness, periphery", pixels.stockPpd, pixels.peripheryPpd, true);  // what QVFR trades
    return lines;
}

}  // namespace simqvfr
