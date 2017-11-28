#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define FP_P         17
#define FP_Q         14
#define FP_F         (1 << FP_Q)
#define FP_F_BY_2    (1 << (FP_Q - 1))

#define INT_TO_FP(n) (n) * FP_F
#define FP_TO_INT(fp) (fp) / FP_F
#define FP_TO_NEAREST_INT(fp) (fp) >= 0 ? \
                                ((fp) + FP_F_BY_2) / FP_F : \
                                ((fp) - FP_F_BY_2) / FP_F

#define FP_ADD(x, y) (x) + (y)
#define FP_SUB(x, y) (x) - (y)
#define FP_MUL(x, y) ((int64_t)(x)) * (y)/FP_F 
#define FP_DIV(x, y) ((int64_t)(x)) * FP_F/(y)
#define FP_ADD_INT(x, n) (x) + INT_TO_FP(n)
#define FP_SUB_INT(x, n) (x) - INT_TO_FP(n)
#define FP_MUL_INT(x, n) ((int64_t)(x)) * (n) 
#define FP_DIV_INT(x, n) ((int64_t)(x)) / (n)

#endif /* threads/fixed-point.h */
