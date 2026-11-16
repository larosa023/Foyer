// Foyer - calculator and unit conversions
#include "calc.h"
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <vector>
#include <map>
#include <algorithm>

namespace calc {

static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- expression parser
// Grammar: expr := term (('+'|'-') term)*
//          term := power (('*'|'/'|'%' mod | implicit) power)*
//          power := unary ('^' power)?        (right associative)
//          unary := ('-'|'+') unary | postfix
//          postfix := primary ('%' percent | '!' factorial)*
//          primary := number | constant | func '(' expr ')' | '(' expr ')'
struct Parser {
    const std::wstring& s;
    size_t i = 0;
    bool ok = true;
    int ops = 0;   // operators and functions seen (a bare number isn't a calculation)
    explicit Parser(const std::wstring& str) : s(str) {}

    void Ws() { while (i < s.size() && iswspace(s[i])) ++i; }
    bool Eat(wchar_t c) { Ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    wchar_t Peek() { Ws(); return i < s.size() ? s[i] : 0; }

    double Expr() {
        double v = Term();
        for (;;) {
            if (Eat(L'+')) { ++ops; v += Term(); }
            else if (Eat(L'-') || Eat(0x2212)) { ++ops; v -= Term(); }
            else return v;
        }
    }
    double Term() {
        double v = Power();
        for (;;) {
            wchar_t c = Peek();
            if (c == L'*' || c == L'x' || c == L'X' || c == 0x00D7) {
                // "x" only counts as multiply between numbers ("3x4"), not in words
                if ((c == L'x' || c == L'X') && !(i + 1 < s.size() && (iswdigit(s[i + 1]) || s[i + 1] == L'(' || s[i + 1] == L' ' || s[i + 1] == L'.'))) return v;
                ++i; ++ops; v *= Power();
            } else if (c == L'/' || c == 0x00F7) {
                ++i; ++ops;
                double d = Power();
                if (d == 0) { ok = false; return 0; }
                v /= d;
            } else if (c == L'(' || iswalpha(c)) {
                // implicit multiplication: 2(3+4), 2pi, 3 sqrt(4)
                size_t save = i;
                if (iswalpha(c) && !IsFuncOrConst()) { i = save; return v; }
                ++ops; v *= Power();
            } else {
                return v;
            }
        }
    }
    double Power() {
        double base = Unary();
        if (Eat(L'^') || (Peek() == L'*' && i + 1 < s.size() && s[i + 1] == L'*' && (i += 2, true))) {
            ++ops;
            double e = Power();
            return std::pow(base, e);
        }
        return base;
    }
    double Unary() {
        if (Eat(L'-') || Eat(0x2212)) return -Unary();
        if (Eat(L'+')) return Unary();
        return Postfix();
    }
    double Postfix() {
        double v = Primary();
        for (;;) {
            if (Peek() == L'%') {
                // "20%" (percent) unless followed by a number ("10 % 3" = modulo)
                size_t save = i;
                ++i;
                Ws();
                if (i < s.size() && (iswdigit(s[i]) || s[i] == L'(' || s[i] == L'.')) {
                    double d = Power();
                    if (d == 0) { ok = false; return 0; }
                    v = std::fmod(v, d);
                    ++ops;
                    continue;
                }
                (void)save;
                v /= 100.0;
                ++ops;
                // "20% of 50"
                Ws();
                if (s.compare(i, 2, L"of") == 0) { i += 2; v *= Power(); }
                continue;
            }
            if (Eat(L'!')) {
                ++ops;
                if (v < 0 || v > 170 || v != std::floor(v)) { ok = false; return 0; }
                double f = 1;
                for (int k = 2; k <= (int)v; ++k) f *= k;
                v = f;
                continue;
            }
            return v;
        }
    }
    bool IsThousandsComma(size_t at) const {
        if (at + 3 >= s.size()) return false;                              // need three characters after
        if (!iswdigit(s[at + 1]) || !iswdigit(s[at + 2]) || !iswdigit(s[at + 3])) return false;
        return at + 4 >= s.size() || !iswdigit(s[at + 4]);                 // and not a fourth digit
    }
    std::wstring Ident() {
        Ws();
        size_t start = i;
        while (i < s.size() && (iswalpha(s[i]) || (i > start && iswdigit(s[i])))) ++i;
        std::wstring id = s.substr(start, i - start);
        for (auto& ch : id) ch = (wchar_t)towlower(ch);
        return id;
    }
    bool IsFuncOrConst() {
        size_t save = i;
        std::wstring id = Ident();
        i = save;
        static const wchar_t* names[] = {L"pi", L"e", L"tau", L"sqrt", L"cbrt", L"sin", L"cos", L"tan", L"asin", L"acos", L"atan",
                                         L"ln", L"log", L"log2", L"abs", L"round", L"floor", L"ceil", L"exp", L"sinh", L"cosh", L"tanh"};
        for (auto n : names) if (id == n) return true;
        return false;
    }
    double Primary() {
        Ws();
        if (i >= s.size()) { ok = false; return 0; }
        if (Eat(L'(')) {
            double v = Expr();
            if (!Eat(L')')) ok = false;
            return v;
        }
        wchar_t c = s[i];
        if (iswdigit(c) || c == L'.') {
            size_t start = i;
            bool dot = false;
            while (i < s.size()) {
                if (iswdigit(s[i])) ++i;
                else if (s[i] == L'.' && !dot) { dot = true; ++i; }
                // thousands separators: "1,250,000" (a comma followed by exactly three digits)
                else if (s[i] == L',' && IsThousandsComma(i)) { ++i; }
                else break;
            }
            std::wstring num;
            for (size_t k = start; k < i; ++k) if (s[k] != L',') num += s[k];
            if (num == L".") { ok = false; return 0; }
            double v = wcstod(num.c_str(), nullptr);
            // scientific notation 1e6
            if (i + 1 < s.size() && (s[i] == L'e' || s[i] == L'E') && (iswdigit(s[i + 1]) || ((s[i + 1] == L'-' || s[i + 1] == L'+') && i + 2 < s.size() && iswdigit(s[i + 2])))) {
                size_t es = ++i;
                if (s[i] == L'-' || s[i] == L'+') ++i;
                while (i < s.size() && iswdigit(s[i])) ++i;
                v *= std::pow(10.0, wcstod(s.substr(es, i - es).c_str(), nullptr));
            }
            return v;
        }
        if (iswalpha(c)) {
            std::wstring id = Ident();
            if (id == L"pi") return kPi;
            if (id == L"tau") return 2 * kPi;
            if (id == L"e") return std::exp(1.0);
            struct F { const wchar_t* n; double (*f)(double); };
            static const F funcs[] = {
                {L"sqrt", [](double x) { return std::sqrt(x); }}, {L"cbrt", [](double x) { return std::cbrt(x); }},
                {L"sin", [](double x) { return std::sin(x); }}, {L"cos", [](double x) { return std::cos(x); }},
                {L"tan", [](double x) { return std::tan(x); }}, {L"asin", [](double x) { return std::asin(x); }},
                {L"acos", [](double x) { return std::acos(x); }}, {L"atan", [](double x) { return std::atan(x); }},
                {L"sinh", [](double x) { return std::sinh(x); }}, {L"cosh", [](double x) { return std::cosh(x); }},
                {L"tanh", [](double x) { return std::tanh(x); }}, {L"ln", [](double x) { return std::log(x); }},
                {L"log", [](double x) { return std::log10(x); }}, {L"log2", [](double x) { return std::log2(x); }},
                {L"abs", [](double x) { return std::fabs(x); }}, {L"round", [](double x) { return std::round(x); }},
                {L"floor", [](double x) { return std::floor(x); }}, {L"ceil", [](double x) { return std::ceil(x); }},
                {L"exp", [](double x) { return std::exp(x); }},
            };
            for (auto& f : funcs) {
                if (id == f.n) {
                    ++ops;
                    double arg = Eat(L'(') ? (Expr() + (Eat(L')') ? 0 : (ok = false, 0))) : Unary();
                    return f.f(arg);
                }
            }
        }
        ok = false;
        return 0;
    }
};

bool EvalExpression(const std::wstring& expr, double& result) {
    Parser p(expr);
    double v = p.Expr();
    p.Ws();
    if (!p.ok || p.i != expr.size() || p.ops == 0) return false;
    if (std::isnan(v) || std::isinf(v)) return false;
    result = v;
    return true;
}

std::wstring FormatNumber(double v) {
    if (v == 0) return L"0";
    double a = std::fabs(v);
    wchar_t buf[64];
    if (a >= 1e15 || a < 1e-9) {
        swprintf(buf, 64, L"%.10g", v);
        return buf;
    }
    swprintf(buf, 64, L"%.10g", v);
    std::wstring s = buf;
    if (s.find(L'e') != std::wstring::npos) {   // %g fell back to exponent; print fixed instead
        swprintf(buf, 64, L"%.10f", v);
        s = buf;
        if (s.find(L'.') != std::wstring::npos) {
            while (!s.empty() && s.back() == L'0') s.pop_back();
            if (!s.empty() && s.back() == L'.') s.pop_back();
        }
    }
    // thousands separators for the integer part
    size_t start = (s[0] == L'-') ? 1 : 0;
    size_t dot = s.find(L'.');
    size_t intEnd = dot == std::wstring::npos ? s.size() : dot;
    if (intEnd - start > 4) {
        std::wstring out = s.substr(0, start);
        std::wstring ip = s.substr(start, intEnd - start);
        int lead = (int)ip.size() % 3;
        for (size_t k = 0; k < ip.size(); ++k) {
            if (k && (int)(k - lead) % 3 == 0) out += L',';
            out += ip[k];
        }
        out += s.substr(intEnd);
        s = out;
    }
    return s;
}

// ---------------------------------------------------------------- units
enum Cat { Length, Area, Volume, Mass, Temperature, Speed, Data, Time, Angle, Pressure, Energy, Power };
static const wchar_t* kCatNames[] = {L"Length", L"Area", L"Volume", L"Weight", L"Temperature", L"Speed", L"Data", L"Time", L"Angle", L"Pressure", L"Energy", L"Power"};

struct Unit { Cat cat; double factor; const wchar_t* symbol; };   // factor to the category's base unit

static const std::map<std::wstring, Unit>& Units() {
    static std::map<std::wstring, Unit> u;
    if (!u.empty()) return u;
    auto add = [&](std::initializer_list<const wchar_t*> names, Cat c, double f, const wchar_t* sym) {
        for (auto n : names) u[n] = {c, f, sym};
    };
    // Length (base: meter)
    add({L"m", L"meter", L"meters", L"metre", L"metres"}, Length, 1, L"m");
    add({L"km", L"kilometer", L"kilometers", L"kilometre", L"kilometres"}, Length, 1000, L"km");
    add({L"cm", L"centimeter", L"centimeters", L"centimetre", L"centimetres"}, Length, 0.01, L"cm");
    add({L"mm", L"millimeter", L"millimeters", L"millimetre", L"millimetres"}, Length, 0.001, L"mm");
    add({L"um", L"micron", L"microns", L"micrometer", L"micrometers"}, Length, 1e-6, L"\x00B5m");
    add({L"in", L"inch", L"inches", L"\""}, Length, 0.0254, L"in");
    add({L"ft", L"foot", L"feet", L"lf", L"'"}, Length, 0.3048, L"ft");
    add({L"yd", L"yard", L"yards"}, Length, 0.9144, L"yd");
    add({L"mi", L"mile", L"miles"}, Length, 1609.344, L"mi");
    add({L"nmi", L"nautical mile", L"nautical miles"}, Length, 1852, L"nmi");
    // Area (base: square meter). sf/sy are the construction shorthands.
    add({L"m2", L"sqm", L"sq m", L"square meter", L"square meters", L"square metre", L"square metres"}, Area, 1, L"m\x00B2");
    add({L"km2", L"sqkm", L"sq km", L"square kilometer", L"square kilometers"}, Area, 1e6, L"km\x00B2");
    add({L"cm2", L"sqcm", L"sq cm", L"square centimeter", L"square centimeters"}, Area, 1e-4, L"cm\x00B2");
    add({L"ft2", L"sqft", L"sq ft", L"sf", L"square foot", L"square feet"}, Area, 0.09290304, L"sq ft");
    add({L"in2", L"sqin", L"sq in", L"square inch", L"square inches"}, Area, 0.00064516, L"sq in");
    add({L"yd2", L"sqyd", L"sq yd", L"sy", L"square yard", L"square yards"}, Area, 0.83612736, L"sq yd");
    add({L"mi2", L"sqmi", L"sq mi", L"square mile", L"square miles"}, Area, 2589988.110336, L"sq mi");
    add({L"acre", L"acres", L"ac"}, Area, 4046.8564224, L"acres");
    add({L"ha", L"hectare", L"hectares"}, Area, 10000, L"ha");
    // Volume (base: liter). cy/cf are construction shorthands for cubic yards/feet.
    add({L"l", L"liter", L"liters", L"litre", L"litres"}, Volume, 1, L"L");
    add({L"ml", L"milliliter", L"milliliters", L"millilitre", L"millilitres"}, Volume, 0.001, L"mL");
    add({L"m3", L"cubic meter", L"cubic meters", L"cubic metre", L"cubic metres", L"cbm"}, Volume, 1000, L"m\x00B3");
    add({L"cm3", L"cc", L"cubic centimeter", L"cubic centimeters"}, Volume, 0.001, L"cm\x00B3");
    add({L"ft3", L"cf", L"cu ft", L"cuft", L"cubic foot", L"cubic feet"}, Volume, 28.316846592, L"cu ft");
    add({L"yd3", L"cy", L"cu yd", L"cuyd", L"cubic yard", L"cubic yards"}, Volume, 764.554857984, L"cu yd");
    add({L"in3", L"cu in", L"cuin", L"cubic inch", L"cubic inches"}, Volume, 0.016387064, L"cu in");
    add({L"gal", L"gallon", L"gallons"}, Volume, 3.785411784, L"gal");
    add({L"qt", L"quart", L"quarts"}, Volume, 0.946352946, L"qt");
    add({L"pt", L"pint", L"pints"}, Volume, 0.473176473, L"pt");
    add({L"cup", L"cups"}, Volume, 0.2365882365, L"cups");
    add({L"floz", L"fl oz", L"fluid ounce", L"fluid ounces"}, Volume, 0.0295735295625, L"fl oz");
    add({L"tbsp", L"tablespoon", L"tablespoons"}, Volume, 0.01478676478125, L"tbsp");
    add({L"tsp", L"teaspoon", L"teaspoons"}, Volume, 0.00492892159375, L"tsp");
    // Mass (base: kilogram)
    add({L"kg", L"kilogram", L"kilograms", L"kilo", L"kilos"}, Mass, 1, L"kg");
    add({L"g", L"gram", L"grams"}, Mass, 0.001, L"g");
    add({L"mg", L"milligram", L"milligrams"}, Mass, 1e-6, L"mg");
    add({L"lb", L"lbs", L"pound", L"pounds"}, Mass, 0.45359237, L"lb");
    add({L"oz", L"ounce", L"ounces"}, Mass, 0.028349523125, L"oz");
    add({L"st", L"stone", L"stones"}, Mass, 6.35029318, L"st");
    add({L"t", L"tonne", L"tonnes", L"metric ton", L"metric tons"}, Mass, 1000, L"t");
    add({L"ton", L"tons", L"short ton", L"short tons"}, Mass, 907.18474, L"tons");
    // Temperature (handled specially; factor unused)
    add({L"c", L"celsius", L"\x00B0" L"c", L"degc"}, Temperature, 0, L"\x00B0" L"C");
    add({L"f", L"fahrenheit", L"\x00B0" L"f", L"degf"}, Temperature, 1, L"\x00B0" L"F");
    add({L"k", L"kelvin"}, Temperature, 2, L"K");
    // Speed (base: m/s)
    add({L"m/s", L"mps", L"meters per second"}, Speed, 1, L"m/s");
    add({L"km/h", L"kmh", L"kph", L"kmph", L"kilometers per hour"}, Speed, 1 / 3.6, L"km/h");
    add({L"mph", L"miles per hour", L"mi/h"}, Speed, 0.44704, L"mph");
    add({L"ft/s", L"fps", L"feet per second"}, Speed, 0.3048, L"ft/s");
    add({L"kn", L"knot", L"knots", L"kt"}, Speed, 1852.0 / 3600.0, L"knots");
    // Data (base: byte)
    add({L"b", L"byte", L"bytes"}, Data, 1, L"B");
    add({L"bit", L"bits"}, Data, 0.125, L"bits");
    add({L"kb", L"kilobyte", L"kilobytes"}, Data, 1e3, L"KB");
    add({L"mb", L"megabyte", L"megabytes"}, Data, 1e6, L"MB");
    add({L"gb", L"gigabyte", L"gigabytes"}, Data, 1e9, L"GB");
    add({L"tb", L"terabyte", L"terabytes"}, Data, 1e12, L"TB");
    add({L"kib", L"kibibyte", L"kibibytes"}, Data, 1024.0, L"KiB");
    add({L"mib", L"mebibyte", L"mebibytes"}, Data, 1048576.0, L"MiB");
    add({L"gib", L"gibibyte", L"gibibytes"}, Data, 1073741824.0, L"GiB");
    add({L"tib", L"tebibyte", L"tebibytes"}, Data, 1099511627776.0, L"TiB");
    // Time (base: second)
    add({L"s", L"sec", L"secs", L"second", L"seconds"}, Time, 1, L"s");
    add({L"ms", L"millisecond", L"milliseconds"}, Time, 0.001, L"ms");
    add({L"min", L"mins", L"minute", L"minutes"}, Time, 60, L"min");
    add({L"h", L"hr", L"hrs", L"hour", L"hours"}, Time, 3600, L"h");
    add({L"day", L"days", L"d"}, Time, 86400, L"days");
    add({L"week", L"weeks", L"wk", L"wks"}, Time, 604800, L"weeks");
    add({L"year", L"years", L"yr", L"yrs"}, Time, 31557600, L"years");
    // Angle (base: degree)
    add({L"deg", L"degree", L"degrees", L"\x00B0"}, Angle, 1, L"\x00B0");
    add({L"rad", L"radian", L"radians"}, Angle, 180.0 / kPi, L"rad");
    // Pressure (base: pascal)
    add({L"pa", L"pascal", L"pascals"}, Pressure, 1, L"Pa");
    add({L"kpa", L"kilopascal", L"kilopascals"}, Pressure, 1000, L"kPa");
    add({L"psi"}, Pressure, 6894.757293168, L"psi");
    add({L"psf"}, Pressure, 47.88025898, L"psf");
    add({L"bar"}, Pressure, 100000, L"bar");
    add({L"atm"}, Pressure, 101325, L"atm");
    // Energy (base: joule)
    add({L"j", L"joule", L"joules"}, Energy, 1, L"J");
    add({L"kj", L"kilojoule", L"kilojoules"}, Energy, 1000, L"kJ");
    add({L"cal", L"calorie", L"calories"}, Energy, 4.184, L"cal");
    add({L"kcal", L"kilocalorie", L"kilocalories"}, Energy, 4184, L"kcal");
    add({L"kwh", L"kilowatt hour", L"kilowatt hours"}, Energy, 3.6e6, L"kWh");
    add({L"btu", L"btus"}, Energy, 1055.05585262, L"BTU");
    // Power (base: watt)
    add({L"w", L"watt", L"watts"}, Power, 1, L"W");
    add({L"kw", L"kilowatt", L"kilowatts"}, Power, 1000, L"kW");
    add({L"hp", L"horsepower"}, Power, 745.69987158, L"hp");
    return u;
}

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static bool LookupUnit(const std::wstring& raw, Unit& out) {
    std::wstring u = Lower(Trim(raw));
    // "sq. ft", "cu.yd"
    std::wstring clean;
    for (wchar_t c : u) if (c != L'.') clean += c;
    while (clean.find(L"  ") != std::wstring::npos) clean.erase(clean.find(L"  "), 1);
    // "ft^2", "ft²"
    for (auto rep : {std::pair<const wchar_t*, const wchar_t*>{L"^2", L"2"}, {L"\x00B2", L"2"}, {L"^3", L"3"}, {L"\x00B3", L"3"}}) {
        size_t p;
        while ((p = clean.find(rep.first)) != std::wstring::npos) clean.replace(p, wcslen(rep.first), rep.second);
    }
    auto& units = Units();
    auto it = units.find(clean);
    if (it == units.end() && clean.size() > 2 && clean.compare(0, 3, L"deg") == 0) it = units.find(clean.substr(3));
    if (it == units.end()) return false;
    out = it->second;
    return true;
}

static double ToBaseTemp(double v, int which) { return which == 0 ? v : which == 1 ? (v - 32) * 5.0 / 9.0 : v - 273.15; }
static double FromBaseTemp(double c, int which) { return which == 0 ? c : which == 1 ? c * 9.0 / 5.0 + 32 : c + 273.15; }

static bool Convert(const std::wstring& q, Answer& out) {
    std::wstring low = Lower(q);
    // split on " to ", " in ", " as ", " -> ", "=" (the last occurrence)
    static const wchar_t* seps[] = {L" to ", L" in ", L" as ", L" into ", L"->", L" = ", L"=>"};
    size_t best = std::wstring::npos, sepLen = 0;
    for (auto sep : seps) {
        size_t p = low.rfind(sep);
        if (p != std::wstring::npos && (best == std::wstring::npos || p > best)) { best = p; sepLen = wcslen(sep); }
    }
    if (best == std::wstring::npos) return false;
    std::wstring left = Trim(q.substr(0, best)), right = Trim(q.substr(best + sepLen));
    if (left.empty() || right.empty()) return false;

    // left = number (or expression) followed by a unit: "12 ft", "12ft", "(3+4) m", "1,250 sf"
    size_t split = left.size();
    while (split > 0 && (iswalpha(left[split - 1]) || left[split - 1] == L'/' || left[split - 1] == L'^' || left[split - 1] == L'\x00B2' ||
                         left[split - 1] == L'\x00B3' || left[split - 1] == L'\x00B0' || left[split - 1] == L'"' || left[split - 1] == L'\'' ||
                         left[split - 1] == L'.' || (iswdigit(left[split - 1]) && split > 1 && (iswalpha(left[split - 2]) || left[split - 2] == L'^')) ||
                         (left[split - 1] == L' ' && split > 1 && iswalpha(left[split - 2]) && split < left.size() && iswalpha(left[split])))) --split;
    // don't swallow a number's decimal point
    while (split < left.size() && (left[split] == L'.' || left[split] == L' ')) ++split;
    std::wstring numText = Trim(left.substr(0, split)), unitText = Trim(left.substr(split));
    if (numText.empty() || unitText.empty()) return false;
    double value;
    if (!EvalExpression(numText + L"+0", value)) return false;
    Unit from, to;
    if (!LookupUnit(unitText, from) || !LookupUnit(right, to) || from.cat != to.cat) return false;
    double result;
    if (from.cat == Temperature) result = FromBaseTemp(ToBaseTemp(value, (int)from.factor), (int)to.factor);
    else result = value * from.factor / to.factor;
    if (std::isnan(result) || std::isinf(result)) return false;
    // round away float noise (0.30000000000000004)
    double r = std::fabs(result) >= 1e-6 ? std::round(result * 1e10) / 1e10 : result;
    out.conversion = true;
    out.value = FormatNumber(r);
    out.display = FormatNumber(value) + L" " + from.symbol + L" = " + out.value + L" " + to.symbol;
    out.detail = std::wstring(kCatNames[from.cat]) + L" conversion";
    return true;
}

bool Evaluate(const std::wstring& query, Answer& out) {
    std::wstring q = Trim(query);
    if (q.empty() || q.size() > 200) return false;
    if (!q.empty() && q[0] == L'=') q = Trim(q.substr(1));
    if (Convert(q, out)) return true;
    // must contain a digit or a known constant to be worth evaluating
    bool digit = false;
    for (wchar_t c : q) if (iswdigit(c)) { digit = true; break; }
    std::wstring lq = Lower(q);
    if (!digit && lq.find(L"pi") == std::wstring::npos) return false;
    double v;
    if (!EvalExpression(q, v)) return false;
    double r = std::fabs(v) >= 1e-6 ? std::round(v * 1e10) / 1e10 : v;
    out.conversion = false;
    out.value = FormatNumber(r);
    out.display = L"= " + out.value;
    out.detail = q;
    return true;
}

} // namespace calc
