static bool trans_g233_dma(DisasContext *ctx, arg_g233_dma *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv g = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_dma(tcg_env, d, s, g);
    return true;
}

static bool trans_g233_gemm(DisasContext *ctx, arg_g233_gemm *a)
{
    TCGv c = get_address(ctx, a->rd, 0);
    TCGv x = get_address(ctx, a->rs1, 0);
    TCGv y = get_address(ctx, a->rs2, 0);

    gen_helper_g233_gemm(tcg_env, c, x, y);
    return true;
}

static bool trans_g233_sort(DisasContext *ctx, arg_g233_sort *a)
{
    TCGv k = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv p = get_address(ctx, a->rs1, 0);
    TCGv n = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_sort(tcg_env, k, p, n);
    return true;
}

static bool trans_g233_vadd(DisasContext *ctx, arg_g233_vadd *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv x = get_address(ctx, a->rs1, 0);
    TCGv y = get_address(ctx, a->rs2, 0);

    gen_helper_g233_vadd(tcg_env, d, x, y);
    return true;
}

static bool trans_g233_crush(DisasContext *ctx, arg_g233_crush *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv n = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_crush(tcg_env, d, s, n);
    return true;
}

static bool trans_g233_expand(DisasContext *ctx, arg_g233_expand *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv n = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_expand(tcg_env, d, s, n);
    return true;
}

static bool trans_g233_vdot(DisasContext *ctx, arg_g233_vdot *a)
{
    TCGv d = dest_gpr(ctx, a->rd);
    TCGv x = get_address(ctx, a->rs1, 0);
    TCGv y = get_address(ctx, a->rs2, 0);

    gen_helper_g233_vdot(d, tcg_env, x, y);
    gen_set_gpr(ctx, a->rd, d);
    return true;
}

static bool trans_g233_vrelu(DisasContext *ctx, arg_g233_vrelu *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv n = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_vrelu(tcg_env, d, s, n);
    return true;
}

static bool trans_g233_vscale(DisasContext *ctx, arg_g233_vscale *a)
{
    TCGv d = get_address(ctx, a->rd, 0);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv c = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_vscale(tcg_env, d, s, c);
    return true;
}

static bool trans_g233_vmax(DisasContext *ctx, arg_g233_vmax *a)
{
    TCGv d = dest_gpr(ctx, a->rd);
    TCGv s = get_address(ctx, a->rs1, 0);
    TCGv n = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_g233_vmax(d, tcg_env, s, n);
    gen_set_gpr(ctx, a->rd, d);
    return true;
}
