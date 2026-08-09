/* LEVIER 1 sur le RUNTIME C++ GNU — étape 1 : le CHEMIN HEUREUX de libstdc++ (sans EH).
 * ARET LIFTE la vraie libstdc++-6.dll de mingw (24 Mo, mesurée §0 : 0 thunk/0 forwarder,
 * importe libgcc+kernel32+msvcrt seulement) PAR-DESSUS libgcc (lifté au préalable) via le
 * loader multi-modules — c'est le 1er cas prouvé d'une DLL liftée qui IMPORTE une AUTRE DLL
 * liftée (libstdc++ -> libgcc). La lacune n°1 mesurée (doc 90 : operator new/delete,
 * std::string, conteneurs — 37-47 % des binaires FOSS).
 *
 * Ce fixture n'exerce que le chemin SANS exception (SSO + alloc tas + append/compare pour
 * std::string ; std::vector push_back/sort/accumulate ; std::sort ; std::map insert/find).
 * Tout passe par du CODE libstdc++ LIFTÉ (operator new -> malloc msvcrt couvert, _M_construct,
 * _Rb_tree, etc.) et sous Wine (l'oracle) la MÊME libstdc++ est chargée à côté de l'exe.
 * Pur & déterministe ⇒ bit-identique. L'EH C++ Itanium (throw/catch à travers frames liftées,
 * les __cxa_ et _Unwind_) est l'ÉTAPE SUIVANTE (brique dédiée) — hors de ce fixture volontairement. */
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <cstdio>

int main() {
    // std::string: SSO, heap alloc (>15 chars), append, compare, find, substr.
    std::string s = "hello";
    s += " world";                          // operator+= -> append
    std::string big(40, 'x');               // forces heap alloc (_M_construct)
    big.replace(10, 5, "MIDDLE");           // _M_replace
    std::size_t pos = s.find("world");
    std::string sub = s.substr(6);
    printf("s=%zu big=%zu pos=%zu sub=%s cmp=%d\n",
           s.size(), big.size(), pos, sub.c_str(), s.compare("hello world"));

    // std::vector<int>: push_back (reallocation growth), sort, accumulate.
    std::vector<int> v;
    for (int i = 0; i < 32; i++) v.push_back((i * 37 + 11) % 100);
    std::sort(v.begin(), v.end());
    long sum = std::accumulate(v.begin(), v.end(), 0L);
    printf("vsz=%zu vmin=%d vmax=%d vsum=%ld\n", v.size(), v.front(), v.back(), sum);

    // std::map<std::string,int>: _Rb_tree insert / balance / find (the measured _Rb_tree_*).
    std::map<std::string, int> m;
    const char* keys[] = {"pear","apple","fig","cherry","date","banana"};
    for (int i = 0; i < 6; i++) m[keys[i]] = (i + 1) * 7;
    long acc = 0;
    for (auto& kv : m) acc = acc * 31 + kv.second;   // ordered traversal (sorted keys)
    printf("msz=%zu first=%s find_fig=%d acc=%ld\n",
           m.size(), m.begin()->first.c_str(), m["fig"], acc);

    // iostream: proves the lifted libstdc++ GLOBAL ctors ran (std::cout/cin/cerr are
    // constructed by libstdc++'s own __CTOR_LIST__ static init, which ARET runs at
    // startup — mingw defers them to __do_global_ctors, which ARET otherwise no-ops).
    // Exercises ostream operator<< for string/int/hex/float/bool + manipulators + endl.
    std::cout << "io: s=" << s << " v0=" << v.front() << " m=" << m["fig"]
              << " hex=" << std::hex << 255 << std::dec
              << " f=" << std::fixed << std::setprecision(2) << 1.5
              << " b=" << std::boolalpha << true << std::endl;
    return 0;
}
