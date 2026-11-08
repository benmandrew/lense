// Alignment sources, unaligned members, memory locations, cache lines and
// field reordering.

struct ByMember { char c; double d; };
struct alignas(16) ByAttribute { char c; };
struct __attribute__((aligned(32))) ByGnuAttribute { int i; };
struct __attribute__((packed)) Packed { char c; int i; };
#pragma pack(push, 2)
struct Pack2 { char c; double d; };
#pragma pack(pop)

// The int sits at offset 0, but a Packed1 can sit at any address.
struct __attribute__((packed)) Packed1 { int i; };
struct HoldsPacked { char c; Packed1 p; };

struct Bits {
  unsigned a : 3, b : 5; // one memory location
  char sep;              // ends it
  unsigned c : 4;
  unsigned : 0;          // ends it
  unsigned d : 4;
};

struct Big {
  char head;
  long long body[9]; // crosses the first cache line
  int tail;
};

struct Shuffled { char a; double b; char c; int d; short e; };
struct Tight { double b; int d; short e; char a, c; };
struct Flexible { char c; long long v; int n; char data[]; };
