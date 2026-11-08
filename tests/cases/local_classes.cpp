// Classes declared inside functions are named from their function, so that
// they never clash with each other or with namespace-scope classes.

struct S { long b; };
void f() { struct S { int a; }; }

struct T0 {
  virtual int m1() { struct T0 { char c; }; return sizeof(T0); }
};

// One instantiation per template argument, nested classes included.
template <int N> int h() { struct S { struct In { char c[N]; } in; }; return sizeof(S); }
int hh = h<1>() + h<2>();

// Two lambdas in one scope are told apart by position.
auto l1 = [] { struct L { short s; }; return L{}; };
auto l2 = [] { struct L { int i; }; return L{}; };

struct Q { struct M { int k(double) { struct K { int z; }; return 0; } }; };
