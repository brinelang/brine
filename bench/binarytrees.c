/* The binary-trees benchmark in C, the same algorithm as binarytrees.br. */
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    struct Node *left, *right;
} Node;

static Node *make(int depth) {
    Node *n = malloc(sizeof(Node));
    if (depth == 0) {
        n->left = n->right = NULL;
    } else {
        n->left = make(depth - 1);
        n->right = make(depth - 1);
    }
    return n;
}

/* counts the nodes; a node with no children stands for Node(Leaf, Leaf) */
static long long check(const Node *t) {
    if (!t->left) return 1;
    return 1 + check(t->left) + check(t->right);
}

static void release(Node *t) {
    if (t->left) {
        release(t->left);
        release(t->right);
    }
    free(t);
}

int main(void) {
    int min_depth = 4, max_depth = 18, stretch = max_depth + 1;
    Node *s = make(stretch);
    printf("stretch tree of depth %d\t check: %lld\n", stretch, check(s));
    release(s);
    Node *long_lived = make(max_depth);
    for (int d = min_depth; d <= max_depth; d += 2) {
        long long iterations = 1LL << (max_depth - d + min_depth), total = 0;
        for (long long i = 0; i < iterations; i++) {
            Node *t = make(d);
            total += check(t);
            release(t);
        }
        printf("%lld\t trees of depth %d\t check: %lld\n", iterations, d, total);
    }
    printf("long lived tree of depth %d\t check: %lld\n", max_depth, check(long_lived));
    release(long_lived);
    return 0;
}
