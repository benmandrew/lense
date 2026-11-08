// A user constructor plus an override of a virtual base method needs a
// vtordisp on Microsoft targets.
struct A { virtual void f(); int a; };
struct B : virtual A { B(); void f() override; int b; };
