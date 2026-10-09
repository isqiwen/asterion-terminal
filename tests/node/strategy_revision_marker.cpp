// Test-only second build of the production service host. Its artifact digest
// differs without changing the journal or application protocol under test.
namespace {
volatile unsigned revision;
struct Marker {
  Marker() { revision = 2; }
} marker;
} // namespace
