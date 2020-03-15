/*
	math.c: this file contains the math efunctions called from
	inside eval_instruction() in interpret.c.
    -- coded by Truilkan 93/02/21
*/

#include <math.h>
#ifdef LATTICE
#include "/lpc_incl.h"
#else
#include "../lpc_incl.h"
#include "../efun_protos.h"
#endif

#ifdef F_COS
void
f_cos PROT((void))
{
    sp->u.real = cos(sp->u.real);
}
#endif

#ifdef F_SIN
void
f_sin PROT((void))
{
    sp->u.real = sin(sp->u.real);
}
#endif
#ifdef F_TAN
void
f_tan PROT((void))
{
    /*
     * maybe should try to check that tan won't blow up (x != (Pi/2 + N*Pi))
     */
    sp->u.real = tan(sp->u.real);
}
#endif

#ifdef F_ASIN
void
f_asin PROT((void))
{
    if (sp->u.real < -1.0) {
        error("math: asin(x) with (x < -1.0)\n");
        return;
    } else if (sp->u.real > 1.0) {
        error("math: asin(x) with (x > 1.0)\n");
        return;
    }
    sp->u.real = asin(sp->u.real);
}
#endif

#ifdef F_ACOS
void
f_acos PROT((void))
{
    if (sp->u.real < -1.0) {
        error("math: acos(x) with (x < -1.0)\n");
        return;
    } else if (sp->u.real > 1.0) {
        error("math: acos(x) with (x > 1.0)\n");
        return;
    }
    sp->u.real = acos(sp->u.real);
}
#endif

#ifdef F_ATAN
void
f_atan PROT((void))
{
    sp->u.real = atan(sp->u.real);
}
#endif

#ifdef F_SQRT
void
f_sqrt PROT((void))
{
    LPC_FLOAT val;

    if(sp->type == T_NUMBER)
      val = (LPC_FLOAT) sp->u.number;
    else
      val = sp->u.real;
      
    if (val < 0.0) {
        error("math: sqrt(x) with (x < 0.0)\n");
        return;
    }
    sp->u.real = (LPC_FLOAT) sqrt(val);
    sp->type = T_REAL;
}
#endif

#ifdef F_LOG
void
f_log PROT((void))
{
    if (sp->u.real <= 0.0) {
        error("math: log(x) with (x <= 0.0)\n");
        return;
    }
    sp->u.real = log(sp->u.real);
}
#endif

#ifdef F_LOG10
void
f_log10 PROT((void))
{
    LPC_FLOAT val;

    if(sp->type == T_NUMBER)
      val = (LPC_FLOAT) sp->u.number;
    else
      val = sp->u.real;

    if (val <= 0.0) {
        error("math: log10(x) with (x <= 0.0)\n");
        return;
    }
    sp->u.real = log10(val);
    sp->type = T_REAL;
}
#endif

#ifdef F_POW
void
f_pow PROT((void))
{
    LPC_FLOAT val, val2;
    
    if((sp-1)->type == T_NUMBER)
      val = (LPC_FLOAT) (sp-1)->u.number;
    else
      val = (sp-1)->u.real;
      
    if(sp->type == T_NUMBER)
      val2 = (LPC_FLOAT) sp->u.number;
    else
      val2 = sp->u.real;

    
    (sp - 1)->u.real = pow(val, val2);
    sp--;
    sp->type = T_REAL;
}
#endif

#ifdef F_EXP
void
f_exp PROT((void))
{
    sp->u.real = exp(sp->u.real);
}
#endif

#ifdef F_FLOOR
void
f_floor PROT((void))
{
    sp->u.real = floor(sp->u.real);
}
#endif

#ifdef F_CEIL
void
f_ceil PROT((void))
{
    sp->u.real = ceil(sp->u.real);
}
#endif
