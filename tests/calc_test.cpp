// Unit tests for the calculator and unit conversions (run on Linux in CI: no Windows code involved)
#include "calc.h"
#include <cstdio>
#include <clocale>
#include <string>

static int failures = 0;

static void Expect(const wchar_t* q, const wchar_t* expected) {
    calc::Answer a;
    bool ok = calc::Evaluate(q, a);
    std::wstring got = ok ? a.display : L"(none)";
    if (got != expected) { printf("FAIL  %ls\n      expected %ls\n      got      %ls\n", q, expected, got.c_str()); ++failures; }
}

int main() {
    setlocale(LC_ALL, "C.UTF-8");
    Expect(L"2+2", L"= 4");
    Expect(L"12*7.5", L"= 90");
    Expect(L"(3+4)*2", L"= 14");
    Expect(L"2^10", L"= 1024");
    Expect(L"10/4", L"= 2.5");
    Expect(L"20% of 50", L"= 10");
    Expect(L"10 % 3", L"= 1");
    Expect(L"sqrt(144)", L"= 12");
    Expect(L"5!", L"= 120");
    Expect(L"1,250,000 * 2", L"= 2,500,000");
    Expect(L"0.1+0.2", L"= 0.3");
    Expect(L"3x4", L"= 12");
    Expect(L"2(3+4)", L"= 14");
    Expect(L"log(1000)", L"= 3");
    Expect(L"1/0", L"(none)");
    Expect(L"42", L"(none)");
    Expect(L"notepad", L"(none)");
    Expect(L"office 365", L"(none)");
    Expect(L"7zip", L"(none)");
    Expect(L"12 ft to m", L"12 ft = 3.6576 m");
    Expect(L"5 cy in cf", L"5 cu yd = 135 cu ft");
    Expect(L"100 c in f", L"100 \x00B0" L"C = 212 \x00B0" L"F");
    Expect(L"0 c to k", L"0 \x00B0" L"C = 273.15 K");
    Expect(L"1 cu yd to cubic feet", L"1 cu yd = 27 cu ft");
    Expect(L"1.5 acres to sq ft", L"1.5 acres = 65,340 sq ft");
    Expect(L"5 ft^2 in in^2", L"5 sq ft = 720 sq in");
    Expect(L"12ft to in", L"12 ft = 144 in");
    Expect(L"10 lbs to kg", L"10 lb = 4.5359237 kg");
    Expect(L"5 m to kg", L"(none)");
    Expect(L"3 to 4", L"(none)");
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("All calculator tests passed\n");
    return 0;
}
