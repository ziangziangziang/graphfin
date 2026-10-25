import { PerspectiveCamera, Vector2 } from "three";

export class CameraRig {
  readonly camera = new PerspectiveCamera(40, 1, 0.1, 40);
  private readonly pointer = new Vector2();
  private readonly current = new Vector2();
  private mobile = false;
  private distance = 10.3;

  resize(width: number, height: number, mobile: boolean) {
    this.mobile = mobile;
    this.camera.aspect = width / Math.max(height, 1);
    this.distance = Math.max(10.3, 6.9 / this.camera.aspect);
    this.camera.position.z = this.distance;
    this.camera.updateProjectionMatrix();
  }

  point(x: number, y: number) {
    this.pointer.set(x, y);
  }

  update(seconds: number, delta: number, still: boolean) {
    const active = !this.mobile && !still;
    const weight = 1 - Math.exp(-delta * 2);
    this.current.x +=
      ((active ? this.pointer.x : 0) - this.current.x) * (still ? 1 : weight);
    this.current.y +=
      ((active ? this.pointer.y : 0) - this.current.y) * (still ? 1 : weight);
    // An actual camera orbit and dolly reveal depth with a stable focal length.
    const time = still ? 0 : seconds;
    const amplitude = this.mobile ? 0.35 : 1;
    this.camera.position.x =
      Math.sin(time * 0.23) * 0.6 * amplitude + this.current.x * 0.34;
    this.camera.position.y =
      Math.sin(time * 0.17) * 0.24 * amplitude + this.current.y * 0.18;
    this.camera.position.z =
      this.distance *
      (1 + (Math.sin(time * 0.19 + 0.4) - Math.sin(0.4)) * 0.018 * amplitude);
    this.camera.lookAt(0, 0.03, 0);
  }
}
