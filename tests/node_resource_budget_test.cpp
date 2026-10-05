#include "resource_budget.hpp"
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::agent;
namespace wire = asterion::node::v1;
TEST(NodeResources, TerminalReservesHeadroomAndBoundsItsPoolsAndWorkerAllowance) {
  ResourceBudget budget({8, 8ULL * 1024 * 1024 * 1024}, true);
  EXPECT_EQ(budget.file_workers, 1);
  Resources used;
  for (const auto kind :
       {wire::MARKET_DATA, wire::DATA_SERVICE, wire::TASK_SERVICE, wire::LIVE_TRADING})
    used += budget.service(kind);
  ASSERT_TRUE(budget.fits(used));
  EXPECT_EQ(budget.worker_slots(used, 2), 2);
  used += ResourceBudget::workers(2);
  EXPECT_TRUE(budget.fits(used));
  EXPECT_EQ(budget.worker_slots(used, 2), 0);
  EXPECT_FALSE(budget.fits(used, budget.service(wire::LIVE_TRADING)));
}
TEST(NodeResources, MemoryAndDiskWorkCanIndependentlyHoldExecutionInTheQueue) {
  ResourceBudget memory({12, 4ULL * 1024 * 1024 * 1024}, false);
  EXPECT_EQ(memory.file_workers, 2);
  const auto task = memory.service(wire::TASK_SERVICE);
  ASSERT_TRUE(memory.fits(task));
  EXPECT_GT(memory.limit.cpu, task.cpu);
  EXPECT_GT(memory.limit.io, task.io);
  EXPECT_EQ(memory.worker_slots(task, 2), 0);

  ResourceBudget disk({32, 64ULL * 1024 * 1024 * 1024}, false);
  auto used = disk.service(wire::MARKET_DATA);
  for (int i = 0; i < 3; ++i)
    used += disk.service(wire::DATA_SERVICE);
  ASSERT_TRUE(disk.fits(used));
  EXPECT_GT(disk.limit.cpu, used.cpu);
  EXPECT_GT(disk.limit.memory_mib, used.memory_mib);
  EXPECT_EQ(disk.worker_slots(used, 2), 0);
}
