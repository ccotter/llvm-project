#include <vector>

void containers() {
    std::vector<int> Vs;

    std::vector<int> Other{std::move(Vs)};
}
