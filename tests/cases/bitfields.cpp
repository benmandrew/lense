// Bitfields, an unnamed bitfield and a zero-width bitfield.
struct Flags {
  unsigned ready : 1;
  unsigned mode : 3;
  unsigned : 2;
  unsigned long long wide : 40;
  int : 0;
  char tail;
};
