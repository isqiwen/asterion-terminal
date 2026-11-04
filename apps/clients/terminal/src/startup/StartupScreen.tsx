import { WindowFrame } from "../host/components/WindowFrame";
import { translate } from "../i18n";
import "./startup.css";

const title = "ASTERION TERMINAL";

// The launch animation: the star draws itself, flashes, and its ring turns
// while the title settles in letter by letter. `leaving` fades it into the
// workbench.
export function StartupScreen({ leaving }: { leaving: boolean }) {
  return (
    <WindowFrame title="Asterion Terminal">
      <main
        className={`startup-screen${leaving ? " leaving" : ""}`}
        aria-label={translate("host", "正在启动")}
        aria-busy="true"
      >
        <svg className="startup-mark" viewBox="-60 -60 120 120" aria-hidden="true">
          <circle className="startup-burst" r="20" />
          <circle className="startup-ring" r="52" pathLength="1" />
          <circle className="startup-orbit" r="52" pathLength="1" />
          <path
            className="startup-star"
            pathLength="1"
            d="M0-40 9.3-9.3 40 0 9.3 9.3 0 40-9.3 9.3-40 0-9.3-9.3Z"
          />
          <path className="startup-axes" d="M0-13V13M-13 0H13" />
        </svg>
        <h1 aria-label={title}>
          {[...title].map((letter, index) => (
            <span key={index} style={{ animationDelay: `${700 + index * 45}ms` }}>
              {letter}
            </span>
          ))}
        </h1>
        <div className="startup-scan">
          <i />
        </div>
      </main>
    </WindowFrame>
  );
}
