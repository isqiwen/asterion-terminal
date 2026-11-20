/**
 * The Asterion mark: a four-point star with its two axes. Its lines keep their
 * width in pixels at every size instead of scaling with the drawing, and a mark
 * too small to hold the axes is drawn without them.
 */
export function BrandMark({ size }: { size: number }) {
  const axes = size >= 32;
  return (
    <svg
      className="ui-icon"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={axes ? 2 : 1.5}
      strokeLinejoin="miter"
      strokeMiterlimit="8"
      aria-hidden="true"
      focusable="false"
    >
      <path
        vectorEffect="non-scaling-stroke"
        d="m12 2 2.8 7.2L22 12l-7.2 2.8L12 22l-2.8-7.2L2 12l7.2-2.8Z"
      />
      {axes && <path vectorEffect="non-scaling-stroke" d="M12 8v8M8 12h8" />}
    </svg>
  );
}
