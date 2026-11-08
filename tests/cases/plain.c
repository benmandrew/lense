/* C structs, including a typedef of an anonymous struct. */
struct node { struct node *next; char tag; int value; };
typedef struct { char a; double b; char c; } padded_t;
struct flex { int len; char data[]; };
