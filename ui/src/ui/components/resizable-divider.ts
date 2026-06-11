import { LitElement, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

/**
 * A draggable divider for resizable split views.
 * Dispatches 'resize' events with { splitRatio: number } detail.
 */
@customElement("resizable-divider")
export class ResizableDivider extends LitElement {
  @property({ type: Number }) splitRatio = 0.6;
  @property({ type: Number }) minRatio = 0.4;
  @property({ type: Number }) maxRatio = 0.7;

  private isDragging = false;
  private startX = 0;
  private startRatio = 0;
  private lastDispatchedRatio: number | null = null;

  static styles = css`
    :host {
      width: 4px;
      cursor: col-resize;
      background: var(--border, #333);
      transition: background 150ms ease-out;
      flex-shrink: 0;
      position: relative;
      touch-action: none;
    }
    :host::before {
      content: "";
      position: absolute;
      top: 0;
      left: -4px;
      right: -4px;
      bottom: 0;
    }
    :host(:hover) {
      background: var(--accent, #007bff);
    }
    :host(.dragging) {
      background: var(--accent, #007bff);
    }
  `;

  render() {
    return nothing;
  }

  connectedCallback() {
    super.connectedCallback();
    this.addEventListener("pointerdown", this.handlePointerDown);
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.removeEventListener("pointerdown", this.handlePointerDown);
    document.removeEventListener("pointermove", this.handlePointerMove);
    document.removeEventListener("pointerup", this.handlePointerUp);
    document.removeEventListener("pointercancel", this.handlePointerUp);
  }

  private handlePointerDown = (e: PointerEvent) => {
    this.isDragging = true;
    this.startX = e.clientX;
    this.startRatio = this.splitRatio;
    this.lastDispatchedRatio = null;
    this.classList.add("dragging");

    document.addEventListener("pointermove", this.handlePointerMove);
    document.addEventListener("pointerup", this.handlePointerUp);
    document.addEventListener("pointercancel", this.handlePointerUp);

    e.preventDefault();
  };

  private handlePointerMove = (e: PointerEvent) => {
    if (!this.isDragging) {
      return;
    }

    const container = this.parentElement;
    if (!container) {
      return;
    }

    const containerWidth = container.getBoundingClientRect().width;
    if (containerWidth <= 0) {
      return;
    }
    const deltaX = e.clientX - this.startX;
    const deltaRatio = deltaX / containerWidth;

    let newRatio = this.startRatio + deltaRatio;
    newRatio = Math.max(this.minRatio, Math.min(this.maxRatio, newRatio));

    if (newRatio === this.lastDispatchedRatio) {
      return;
    }
    this.lastDispatchedRatio = newRatio;

    this.dispatchEvent(
      new CustomEvent("resize", {
        detail: { splitRatio: newRatio },
        bubbles: true,
        composed: true,
      }),
    );
  };

  private handlePointerUp = () => {
    this.isDragging = false;
    this.classList.remove("dragging");

    document.removeEventListener("pointermove", this.handlePointerMove);
    document.removeEventListener("pointerup", this.handlePointerUp);
    document.removeEventListener("pointercancel", this.handlePointerUp);
  };
}

declare global {
  interface HTMLElementTagNameMap {
    "resizable-divider": ResizableDivider;
  }
}
