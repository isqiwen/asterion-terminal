export function FlowSteps({ labels, current }: { labels: string[]; current: number }) {
  return (
    <ol className="flow-steps">
      {labels.map((label, index) => (
        <li
          key={label}
          aria-current={current === index ? "step" : undefined}
          data-done={index < current || undefined}
        >
          <span>{index + 1}</span>
          {label}
        </li>
      ))}
    </ol>
  );
}
