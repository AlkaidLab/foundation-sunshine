#include <stdexcept>
#include <iostream>
int main() { try { throw std::runtime_error("expected"); } catch (const std::exception& e) { std::cout << e.what() << '\n'; return 0; } }
