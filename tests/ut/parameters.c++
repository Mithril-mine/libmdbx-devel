/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

namespace {

using geometry = mdbx::geometry;
using reclaiming_options = mdbx::reclaiming_options;
using operate_options = mdbx::operate_options;
using operate_parameters = mdbx::operate_parameters;
using create_parameters = mdbx::create_parameters;

// All fluent setters must be noexcept and chainable on the same instance.
static_assert(noexcept(geometry().set_size_lower(1)), "set_size_lower() must be noexcept");
static_assert(noexcept(geometry().set_size_now(1)), "set_size_now() must be noexcept");
static_assert(noexcept(geometry().set_size_upper(1)), "set_size_upper() must be noexcept");
static_assert(noexcept(geometry().set_growth_step(1)), "set_growth_step() must be noexcept");
static_assert(noexcept(geometry().set_shrink_threshold(1)), "set_shrink_threshold() must be noexcept");
static_assert(noexcept(geometry().set_pagesize(1)), "set_pagesize() must be noexcept");
static_assert(noexcept(reclaiming_options().set_lifo()), "set_lifo() must be noexcept");
static_assert(noexcept(operate_options().set_no_sticky_threads()), "set_no_sticky_threads() must be noexcept");
static_assert(noexcept(operate_options().set_nested_transactions()), "set_nested_transactions() must be noexcept");
static_assert(noexcept(operate_options().set_exclusive()), "set_exclusive() must be noexcept");
static_assert(noexcept(operate_options().set_disable_readahead()), "set_disable_readahead() must be noexcept");
static_assert(noexcept(operate_options().set_disable_clear_memory()), "set_disable_clear_memory() must be noexcept");
static_assert(noexcept(operate_options().set_enable_validation()), "set_enable_validation() must be noexcept");
static_assert(noexcept(operate_parameters().set_max_maps(1)), "set_max_maps() must be noexcept");
static_assert(noexcept(operate_parameters().set_max_readers(1)), "set_max_readers() must be noexcept");
static_assert(noexcept(operate_parameters().set_mode(mdbx::env::mode::readonly)),
              "set_mode() must be noexcept");
static_assert(noexcept(operate_parameters().set_durability(mdbx::env::durability::whole_fragile)),
              "set_durability() must be noexcept");
static_assert(noexcept(operate_parameters().set_reclaiming(reclaiming_options())),
              "set_reclaiming() must be noexcept");
static_assert(noexcept(operate_parameters().set_options(operate_options())), "set_options() must be noexcept");
static_assert(noexcept(operate_parameters().readonly()), "readonly() must be noexcept");
static_assert(noexcept(operate_parameters().write_file_io()), "write_file_io() must be noexcept");
static_assert(noexcept(operate_parameters().write_mapped_io()), "write_mapped_io() must be noexcept");
static_assert(noexcept(operate_parameters().robust_synchronous()), "robust_synchronous() must be noexcept");
static_assert(noexcept(operate_parameters().half_synchronous_weak_last()),
              "half_synchronous_weak_last() must be noexcept");
static_assert(noexcept(operate_parameters().lazy_weak_tail()), "lazy_weak_tail() must be noexcept");
static_assert(noexcept(operate_parameters().whole_fragile()), "whole_fragile() must be noexcept");
static_assert(noexcept(operate_parameters().nested_transactions()), "nested_transactions() must be noexcept");
static_assert(noexcept(operate_parameters().lifo()), "lifo() must be noexcept");
static_assert(noexcept(operate_parameters().exclusive()), "exclusive() must be noexcept");
static_assert(noexcept(operate_parameters().enable_validation()), "enable_validation() must be noexcept");
static_assert(noexcept(create_parameters().set_geometry(geometry())), "set_geometry() must be noexcept");
static_assert(noexcept(create_parameters().set_file_mode_bits(0640)), "set_file_mode_bits() must be noexcept");
static_assert(noexcept(create_parameters().set_use_subdirectory()), "set_use_subdirectory() must be noexcept");

void test_geometry_setters() {
  geometry geo;
  EXPECT_EQ(geo.size_lower, geometry::default_value);
  EXPECT_EQ(geo.size_now, geometry::default_value);
  EXPECT_EQ(geo.size_upper, geometry::default_value);
  EXPECT_EQ(geo.growth_step, geometry::default_value);
  EXPECT_EQ(geo.shrink_threshold, geometry::default_value);
  EXPECT_EQ(geo.pagesize, geometry::default_value);

  // chained per-field setters mutate in place and return *this
  geometry &ref = geo.set_size_lower(1 * geometry::GiB)
                      .set_size_now(2 * geometry::GiB)
                      .set_size_upper(8 * geometry::GiB)
                      .set_growth_step(256 * geometry::MiB)
                      .set_shrink_threshold(512 * geometry::MiB)
                      .set_pagesize(4 * geometry::KiB);
  EXPECT_EQ(&ref, &geo);
  EXPECT_EQ(geo.size_lower, 1 * geometry::GiB);
  EXPECT_EQ(geo.size_now, 2 * geometry::GiB);
  EXPECT_EQ(geo.size_upper, 8 * geometry::GiB);
  EXPECT_EQ(geo.growth_step, 256 * geometry::MiB);
  EXPECT_EQ(geo.shrink_threshold, 512 * geometry::MiB);
  EXPECT_EQ(geo.pagesize, 4 * geometry::KiB);

  // make_fixed/make_dynamic still override everything and remain chainable
  EXPECT_EQ(&geo.make_fixed(42 * geometry::MiB), &geo);
  EXPECT_EQ(geo.size_lower, 42 * geometry::MiB);
  EXPECT_EQ(geo.size_now, 42 * geometry::MiB);
  EXPECT_EQ(geo.size_upper, 42 * geometry::MiB);
  EXPECT_EQ(geo.growth_step, 0);
  EXPECT_EQ(geo.shrink_threshold, 0);

  EXPECT_EQ(&geo.make_dynamic(21 * geometry::MiB, geometry::GiB / 4), &geo);
  EXPECT_EQ(geo.size_lower, 21 * geometry::MiB);
  EXPECT_EQ(geo.size_now, 21 * geometry::MiB);
  EXPECT_EQ(geo.size_upper, geometry::GiB / 4);
  EXPECT_EQ(geo.growth_step, geometry::default_value);
  EXPECT_EQ(geo.shrink_threshold, geometry::default_value);
}

void test_reclaiming_options_setters() {
  reclaiming_options ro;
  EXPECT_FALSE(ro.lifo);
  EXPECT_EQ(&ro.set_lifo(), &ro);
  EXPECT_TRUE(ro.lifo);
  EXPECT_EQ(&ro.set_lifo(false), &ro);
  EXPECT_FALSE(ro.lifo);
  EXPECT_TRUE(ro.set_lifo(true).lifo);
}

void test_operate_options_setters() {
  operate_options oo;
  EXPECT_FALSE(oo.no_sticky_threads);
  EXPECT_FALSE(oo.nested_transactions);
  EXPECT_FALSE(oo.exclusive);
  EXPECT_FALSE(oo.disable_readahead);
  EXPECT_FALSE(oo.disable_clear_memory);
  EXPECT_FALSE(oo.enable_validation);

  operate_options &ref = oo.set_no_sticky_threads()
                             .set_nested_transactions()
                             .set_exclusive()
                             .set_disable_readahead()
                             .set_disable_clear_memory()
                             .set_enable_validation();
  EXPECT_EQ(&ref, &oo);
  EXPECT_TRUE(oo.no_sticky_threads);
  EXPECT_TRUE(oo.nested_transactions);
  EXPECT_TRUE(oo.exclusive);
  EXPECT_TRUE(oo.disable_readahead);
  EXPECT_TRUE(oo.disable_clear_memory);
  EXPECT_TRUE(oo.enable_validation);

  EXPECT_FALSE(oo.set_no_sticky_threads(false).no_sticky_threads);
  EXPECT_FALSE(oo.set_enable_validation(false).enable_validation);
}

void test_operate_parameters_setters() {
  operate_parameters op;
  EXPECT_EQ(op.max_maps, 0u);
  EXPECT_EQ(op.max_readers, 0u);
  EXPECT_EQ(op.mode, mdbx::env::mode::write_mapped_io);
  EXPECT_EQ(op.durability, mdbx::env::durability::robust_synchronous);
  EXPECT_FALSE(op.reclaiming.lifo);
  EXPECT_FALSE(op.options.exclusive);

  operate_parameters &ref =
      op.set_max_maps(7)
          .set_max_readers(4)
          .set_mode(mdbx::env::mode::readonly)
          .set_durability(mdbx::env::durability::half_synchronous_weak_last)
          .set_reclaiming(reclaiming_options().set_lifo())
          .set_options(operate_options().set_exclusive().set_enable_validation());
  EXPECT_EQ(&ref, &op);
  EXPECT_EQ(op.max_maps, 7u);
  EXPECT_EQ(op.max_readers, 4u);
  EXPECT_EQ(op.mode, mdbx::env::mode::readonly);
  EXPECT_EQ(op.durability, mdbx::env::durability::half_synchronous_weak_last);
  EXPECT_TRUE(op.reclaiming.lifo);
  EXPECT_TRUE(op.options.exclusive);
  EXPECT_TRUE(op.options.enable_validation);

  // convenience mode/durability shortcuts
  EXPECT_EQ(op.readonly().mode, mdbx::env::mode::readonly);
  EXPECT_EQ(op.write_file_io().mode, mdbx::env::mode::write_file_io);
  EXPECT_EQ(op.write_mapped_io().mode, mdbx::env::mode::write_mapped_io);
  EXPECT_EQ(op.robust_synchronous().durability, mdbx::env::durability::robust_synchronous);
  EXPECT_EQ(op.half_synchronous_weak_last().durability, mdbx::env::durability::half_synchronous_weak_last);
  EXPECT_EQ(op.lazy_weak_tail().durability, mdbx::env::durability::lazy_weak_tail);
  EXPECT_EQ(op.whole_fragile().durability, mdbx::env::durability::whole_fragile);

  // convenience option/reclaiming shortcuts
  EXPECT_TRUE(op.nested_transactions().options.nested_transactions);
  EXPECT_FALSE(op.nested_transactions(false).options.nested_transactions);
  EXPECT_TRUE(op.lifo().reclaiming.lifo);
  EXPECT_FALSE(op.lifo(false).reclaiming.lifo);
  EXPECT_TRUE(op.exclusive().options.exclusive);
  EXPECT_TRUE(op.no_sticky_threads().options.no_sticky_threads);
  EXPECT_TRUE(op.disable_readahead().options.disable_readahead);
  EXPECT_TRUE(op.disable_clear_memory().options.disable_clear_memory);
  EXPECT_TRUE(op.enable_validation().options.enable_validation);
}

void test_flags_equivalence() {
  const MDBX_env_flags_t writemap =
      mdbx::env::operate_parameters().set_mode(mdbx::env::mode::write_mapped_io).make_flags();
  EXPECT_NE(0u, writemap & MDBX_WRITEMAP);

  const MDBX_env_flags_t readonly =
      mdbx::env::operate_parameters().readonly().make_flags();
  EXPECT_NE(0u, readonly & MDBX_RDONLY);
  EXPECT_EQ(0u, readonly & MDBX_WRITEMAP);

  const MDBX_env_flags_t flags = mdbx::env::operate_parameters()
                                     .set_max_maps(7)
                                     .set_max_readers(4)
                                     .write_mapped_io()
                                     .half_synchronous_weak_last()
                                     .lifo()
                                     .exclusive()
                                     .no_sticky_threads()
                                     .disable_readahead()
                                     .disable_clear_memory()
                                     .enable_validation()
                                     .make_flags();
  EXPECT_NE(0u, flags & MDBX_WRITEMAP);
  EXPECT_NE(0u, flags & MDBX_NOMETASYNC);
  EXPECT_NE(0u, flags & MDBX_LIFORECLAIM);
  EXPECT_NE(0u, flags & MDBX_EXCLUSIVE);
  EXPECT_NE(0u, flags & MDBX_NOSTICKYTHREADS);
  EXPECT_NE(0u, flags & MDBX_NORDAHEAD);
  EXPECT_NE(0u, flags & MDBX_NOMEMINIT);
  EXPECT_NE(0u, flags & MDBX_VALIDATION);
  EXPECT_NE(0u, flags & MDBX_ACCEDE); // default accede=true

  const MDBX_env_flags_t fragile =
      mdbx::env::operate_parameters().write_file_io().whole_fragile().lifo(false).make_flags();
  EXPECT_EQ(0u, fragile & MDBX_WRITEMAP);
  EXPECT_NE(0u, fragile & MDBX_UTTERLY_NOSYNC);
  EXPECT_EQ(0u, fragile & MDBX_LIFORECLAIM);

  const MDBX_env_flags_t subdir = mdbx::env::operate_parameters()
                                      .set_mode(mdbx::env::mode::write_mapped_io)
                                      .make_flags(/*accede=*/true, /*use_subdirectory=*/true);
  EXPECT_EQ(0u, subdir & MDBX_NOSUBDIR);
}

void test_create_parameters_setters() {
  create_parameters cp;
  EXPECT_EQ(cp.file_mode_bits, mdbx_mode_t(0640));
  EXPECT_FALSE(cp.use_subdirectory);
  EXPECT_EQ(cp.geometry.pagesize, geometry::default_value);

  const geometry geo = geometry().set_pagesize(16 * geometry::KiB).make_dynamic(1 * geometry::MiB, 1 * geometry::GiB);
  create_parameters &ref = cp.set_geometry(geo).set_file_mode_bits(0600).set_use_subdirectory();
  EXPECT_EQ(&ref, &cp);
  EXPECT_EQ(cp.geometry.pagesize, 16 * geometry::KiB);
  EXPECT_EQ(cp.geometry.size_lower, 1 * geometry::MiB);
  EXPECT_EQ(cp.geometry.size_upper, 1 * geometry::GiB);
  EXPECT_EQ(cp.file_mode_bits, mdbx_mode_t(0600));
  EXPECT_TRUE(cp.use_subdirectory);
  EXPECT_FALSE(cp.set_use_subdirectory(false).use_subdirectory);
}

void test_factories() {
  const geometry fixed = geometry::fixed(42 * geometry::MiB);
  EXPECT_EQ(fixed.size_lower, 42 * geometry::MiB);
  EXPECT_EQ(fixed.size_now, 42 * geometry::MiB);
  EXPECT_EQ(fixed.size_upper, 42 * geometry::MiB);
  EXPECT_EQ(fixed.growth_step, 0);
  EXPECT_EQ(fixed.shrink_threshold, 0);

  const geometry dynamic = geometry::dynamic(21 * geometry::MiB, geometry::GiB / 4);
  EXPECT_EQ(dynamic.size_lower, 21 * geometry::MiB);
  EXPECT_EQ(dynamic.size_now, 21 * geometry::MiB);
  EXPECT_EQ(dynamic.size_upper, geometry::GiB / 4);
  EXPECT_EQ(dynamic.growth_step, geometry::default_value);

  const operate_parameters read_only = operate_parameters::read_only();
  EXPECT_EQ(read_only.mode, mdbx::env::mode::readonly);

  const operate_parameters safe = operate_parameters::safe_write();
  EXPECT_EQ(safe.mode, mdbx::env::mode::write_mapped_io);
  EXPECT_EQ(safe.durability, mdbx::env::durability::robust_synchronous);

  const operate_parameters lazy = operate_parameters::lazy_write();
  EXPECT_EQ(lazy.mode, mdbx::env::mode::write_mapped_io);
  EXPECT_EQ(lazy.durability, mdbx::env::durability::lazy_weak_tail);

  const operate_parameters fragile = operate_parameters::fragile_write();
  EXPECT_EQ(fragile.mode, mdbx::env::mode::write_mapped_io);
  EXPECT_EQ(fragile.durability, mdbx::env::durability::whole_fragile);
}

void test_comparisons() {
  const geometry geo_a = geometry().set_size_lower(1).set_pagesize(4 * geometry::KiB);
  const geometry geo_b = geometry().set_size_lower(1).set_pagesize(4 * geometry::KiB);
  const geometry geo_c = geometry().set_size_lower(2).set_pagesize(4 * geometry::KiB);
  EXPECT_TRUE(geo_a == geo_b);
  EXPECT_FALSE(geo_a != geo_b);
  EXPECT_TRUE(geo_a != geo_c);
#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
  EXPECT_TRUE(geo_a < geo_c);
  EXPECT_TRUE(geo_c > geo_a);
  EXPECT_FALSE(geo_c <= geo_a);
#endif /* three-way comparison */

  const reclaiming_options ro_a = reclaiming_options().set_lifo();
  const reclaiming_options ro_b = reclaiming_options().set_lifo();
  const reclaiming_options ro_c = reclaiming_options();
  EXPECT_TRUE(ro_a == ro_b);
  EXPECT_TRUE(ro_a != ro_c);

  const operate_options oo_a = operate_options().set_exclusive().set_enable_validation();
  const operate_options oo_b = operate_options().set_exclusive().set_enable_validation();
  const operate_options oo_c = operate_options().set_exclusive();
  EXPECT_TRUE(oo_a == oo_b);
  EXPECT_TRUE(oo_a != oo_c);

  const operate_parameters op_a = operate_parameters().set_max_maps(7).readonly().lifo();
  const operate_parameters op_b = operate_parameters().set_max_maps(7).readonly().lifo();
  const operate_parameters op_c = operate_parameters().set_max_maps(8).readonly().lifo();
  EXPECT_TRUE(op_a == op_b);
  EXPECT_TRUE(op_a != op_c);

  const create_parameters cp_a = create_parameters().set_file_mode_bits(0600).set_use_subdirectory();
  const create_parameters cp_b = create_parameters().set_file_mode_bits(0600).set_use_subdirectory();
  const create_parameters cp_c = create_parameters().set_file_mode_bits(0600);
  EXPECT_TRUE(cp_a == cp_b);
  EXPECT_TRUE(cp_a != cp_c);
}

#if defined(__cpp_constexpr) && __cpp_constexpr >= 201304L
constexpr operate_parameters compile_time_params() {
  operate_parameters op;
  op.set_max_maps(9).set_max_readers(2).readonly().whole_fragile().lifo();
  return op;
}

static constexpr operate_parameters cxx14_params = compile_time_params();
static_assert(cxx14_params.max_maps == 9, "constexpr set_max_maps() mismatch");
static_assert(cxx14_params.max_readers == 2, "constexpr set_max_readers() mismatch");
static_assert(cxx14_params.mode == mdbx::env::mode::readonly, "constexpr readonly() mismatch");
static_assert(cxx14_params.durability == mdbx::env::durability::whole_fragile, "constexpr whole_fragile() mismatch");
static_assert(cxx14_params.reclaiming.lifo, "constexpr lifo() mismatch");

static constexpr geometry cxx14_fixed = geometry::fixed(1 * geometry::MiB);
static_assert(cxx14_fixed.size_lower == 1 * geometry::MiB, "constexpr geometry::fixed() mismatch");
static_assert(cxx14_fixed.growth_step == 0, "constexpr geometry::fixed() mismatch");

static constexpr operate_parameters cxx14_read_only = operate_parameters::read_only();
static_assert(cxx14_read_only.mode == mdbx::env::mode::readonly, "constexpr read_only() mismatch");
#endif /* constexpr setters */

TEST(ut_parameters, all) {
  test_geometry_setters();
  test_reclaiming_options_setters();
  test_operate_options_setters();
  test_operate_parameters_setters();
  test_flags_equivalence();
  test_create_parameters_setters();
  test_factories();
  test_comparisons();

  // end-to-end: fluent parameters drive a real environment
  const mdbx::path testdb = "test-parameters";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, create_parameters().set_geometry(geometry().set_pagesize(4 * geometry::KiB)),
                        operate_parameters().set_max_maps(8).set_max_readers(3).write_mapped_io().lifo());
  EXPECT_EQ(env.max_maps(), 8u);
  EXPECT_GE(env.max_readers(), 3u); // libmdbx may clamp-up to the lck table size
  EXPECT_TRUE(env.is_writemap());
  EXPECT_TRUE(env.get_reclaiming().lifo);
  env.close();
  mdbx::env::remove(testdb);
}

} // namespace