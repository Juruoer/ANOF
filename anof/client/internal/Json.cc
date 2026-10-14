#include <anof/client/internal/Json.h>
namespace anof::client::internal {
bool validTimestamp(std::string_view s) noexcept {
    if (s.size() != 27 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != '.' || s[26] != 'Z') return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19 || i == 26) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    auto number = [&](std::size_t at, std::size_t n) { int v = 0; for (auto i = at; i < at + n; ++i) v = v * 10 + s[i] - '0'; return v; };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    constexpr int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (year == 0 || month < 1 || month > 12) return false;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month] + (month == 2 && leap ? 1 : 0) && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}
}  // namespace anof::client::internal
