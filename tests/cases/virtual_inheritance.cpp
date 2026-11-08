// Diamond with virtual bases, a covariant return and thunks.
struct A { virtual ~A(); virtual A *clone() const; int a; };
struct B : virtual A { void f(); virtual void g(); int b; };
struct C : virtual A { C *clone() const override; int c; };
struct D : B, C { D *clone() const override; void g() override; int d; };
