// Splits items, kept in order, into at most `count` columns so the tallest
// column is as short as it can be; among equally tall splits, the fewest columns.
export function boardColumns(heights: number[], count: number): number[][] {
  const total = (column: number[]) => column.reduce((sum, index) => sum + heights[index]!, 0);
  let best: number[][] = [];
  let tallest = Infinity;
  const split = (start: number, done: number[][]) => {
    if (start === heights.length) {
      const height = Math.max(...done.map(total));
      if (height < tallest || (height === tallest && done.length < best.length)) {
        best = done;
        tallest = height;
      }
      return;
    }
    if (done.length === count) return;
    for (let end = start + 1; end <= heights.length; ++end)
      split(end, [...done, Array.from({ length: end - start }, (_, i) => start + i)]);
  };
  split(0, []);
  return best;
}
