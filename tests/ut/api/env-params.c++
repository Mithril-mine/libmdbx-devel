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

TEST(ut_env_params, geometry_value_types) {
  using mdbx::geometry;

  const geometry g(1, 2, 3, 4, 5, 4096);
  EXPECT_EQ(g.size_lower, 1);
  EXPECT_EQ(g.size_now, 2);
  EXPECT_EQ(g.size_upper, 3);
  EXPECT_EQ(g.growth_step, 4);
  EXPECT_EQ(g.shrink_threshold, 5);
  EXPECT_EQ(g.pagesize, 4096);

  geometry g2;
  g2.set_size_lower(64 << 20).set_size_upper(1024ull << 20).set_growth_step(16ull << 20)
      .set_shrink_threshold(128ull << 20).set_pagesize(16384);
  EXPECT_EQ(g2.size_lower, 64 << 20);
  EXPECT_EQ(g2.size_upper, 1024ull << 20);
  EXPECT_EQ(g2.growth_step, 16ull << 20);
  EXPECT_EQ(g2.shrink_threshold, 128ull << 20);
  EXPECT_EQ(g2.pagesize, 16384);
  EXPECT_NE(g, g2);

  const auto fixed = geometry::fixed(16ull << 20);
  EXPECT_EQ(fixed.size_lower, 16ull << 20);
  EXPECT_EQ(fixed.size_now, 16ull << 20);
  EXPECT_EQ(fixed.size_upper, 16ull << 20);
  EXPECT_EQ(fixed.growth_step, 0);
  EXPECT_EQ(fixed.shrink_threshold, 0);

  const auto dyn = geometry::dynamic();
  EXPECT_EQ(dyn.size_lower, geometry::default_value);
  EXPECT_EQ(dyn.size_upper, geometry::default_value);
  EXPECT_EQ(dyn.growth_step, geometry::default_value);

  const auto dyn_bounded = geometry::dynamic(8ull << 20, 256ull << 20);
  EXPECT_EQ(dyn_bounded.size_lower, 8ull << 20);
  EXPECT_EQ(dyn_bounded.size_upper, 256ull << 20);

  geometry copy = g2;
  EXPECT_EQ(copy, g2);
  copy.set_pagesize(8192);
  EXPECT_NE(copy, g2);
}

TEST(ut_env_params, mode_and_durability_enums) {
    EXPECT_EQ(static_cast<unsigned>(mdbx::env::mode::readonly), 0u);
  EXPECT_EQ(static_cast<unsigned>(mdbx::env::mode::write_file_io), static_cast<unsigned>(mdbx::env::mode::nested_transactions));
  EXPECT_NE(static_cast<unsigned>(mdbx::env::mode::write_mapped_io), static_cast<unsigned>(mdbx::env::mode::readonly));
  EXPECT_EQ(static_cast<unsigned>(mdbx::env::durability::robust_synchronous), 0u);
  EXPECT_NE(static_cast<unsigned>(mdbx::env::durability::half_synchronous_weak_last),
            static_cast<unsigned>(mdbx::env::durability::whole_fragile));
}

TEST(ut_env_params, reclaiming_and_operate_options) {
  using mdbx::reclaiming_options;
  using mdbx::operate_options;

  reclaiming_options r;
  EXPECT_FALSE(r.lifo);
  r.set_lifo();
  EXPECT_TRUE(r.lifo);
  r.set_lifo(false);
  EXPECT_FALSE(r.lifo);

  operate_options o;
  EXPECT_FALSE(o.no_sticky_threads);
  EXPECT_FALSE(o.nested_transactions);
  EXPECT_FALSE(o.exclusive);
  EXPECT_FALSE(o.disable_readahead);
  EXPECT_FALSE(o.disable_clear_memory);
  EXPECT_FALSE(o.enable_validation);
  o.set_no_sticky_threads().set_nested_transactions().set_exclusive().set_disable_readahead()
      .set_disable_clear_memory().set_enable_validation();
  EXPECT_TRUE(o.no_sticky_threads);
  EXPECT_TRUE(o.nested_transactions);
  EXPECT_TRUE(o.exclusive);
  EXPECT_TRUE(o.disable_readahead);
  EXPECT_TRUE(o.disable_clear_memory);
  EXPECT_TRUE(o.enable_validation);
}

TEST(ut_env_params, operate_and_create_parameters_fluent) {
  using mdbx::operate_parameters;
  using mdbx::create_parameters;

  const operate_parameters p = operate_parameters().set_max_maps(8).set_max_readers(16)
                                   .set_mode(mdbx::env::mode::write_file_io).set_durability(mdbx::env::durability::lazy_weak_tail)
                                   .lifo().no_sticky_threads().exclusive();
  EXPECT_EQ(p.max_maps, 8u);
  EXPECT_EQ(p.max_readers, 16u);
  EXPECT_EQ(p.mode, mdbx::env::mode::write_file_io);
  EXPECT_EQ(p.durability, mdbx::env::durability::lazy_weak_tail);
  EXPECT_TRUE(p.reclaiming.lifo);
  EXPECT_TRUE(p.options.no_sticky_threads);
  EXPECT_TRUE(p.options.exclusive);
  EXPECT_FALSE(p.options.disable_readahead);

  const operate_parameters sugar = operate_parameters().readonly().whole_fragile().nested_transactions(true);
  EXPECT_EQ(sugar.mode, mdbx::env::mode::readonly);
  EXPECT_EQ(sugar.durability, mdbx::env::durability::whole_fragile);
  EXPECT_TRUE(sugar.options.nested_transactions);

  const create_parameters c = create_parameters().set_geometry(mdbx::geometry::fixed(16ull << 20));
  EXPECT_EQ(c.geometry.size_upper, 16ull << 20);

  operate_parameters copy = p;
  EXPECT_EQ(copy, p);
  copy.set_max_maps(64);
  EXPECT_NE(copy, p);
}

TEST(ut_env_params, env_roundtrip) {
  const mdbx::path testdb = "test-env-params";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(
      testdb, mdbx::create_parameters(),
      mdbx::operate_parameters().set_max_maps(8).write_file_io().half_synchronous_weak_last().lifo()
          .no_sticky_threads());
  EXPECT_EQ(env.get_mode(), mdbx::env::mode::write_file_io);
  EXPECT_EQ(env.get_durability(), mdbx::env::durability::half_synchronous_weak_last);
  EXPECT_TRUE(env.get_reclaiming().lifo);
  EXPECT_TRUE(env.get_options().no_sticky_threads);
  EXPECT_FALSE(env.get_options().exclusive);
  EXPECT_GE(env.max_maps(), 8u);
  env.close();
  mdbx::env::remove(testdb);
}