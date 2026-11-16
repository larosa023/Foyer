// Foyer - calculator and unit conversions for the search box (no Windows dependencies)
#pragma once
#include <string>

namespace calc {

struct Answer {
    std::wstring display;   // what to show, e.g. "= 90" or "12 ft = 3.6576 m"
    std::wstring value;     // what Enter copies, e.g. "90" or "3.6576"
    std::wstring detail;    // second line, e.g. "Length" or the normalized expression
    bool conversion = false;
};

// True when `query` is a math expression (with at least one operator or function)
// or a unit conversion like "12 ft to m", "5 cy in cf", "72 f in c".
bool Evaluate(const std::wstring& query, Answer& out);

// Exposed for tests
bool EvalExpression(const std::wstring& expr, double& result);
std::wstring FormatNumber(double v);

} // namespace calc
