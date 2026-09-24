import { PerspectiveCamera, Vector2 } from "three";

export class CameraRig {
  readonly camera = new PerspectiveCamera(37, 1, 0.1, 40);
  private readonly pointer = new Vector2();
  private readonly current = new Vector2();
  private mobile = false;

  resize(width: number, height: number, mobile: boolean) {
    this.mobile = mobile;
    this.camera.aspect = width / Math.max(height, 1);
    // Fit both portrait and landscape containers without cropping the lattice.
    this.camera.position.z = Math.max(10.3, 7.4 / this.camera.aspect);
    this.camera.updateProjectionMatrix();
  }

  point(x: number, y: number) {
    this.pointer.set(x, y);
  }

  update(delta: number, still: boolean) {
    const active = !this.mobile && !still;
    const weight = 1 - Math.exp(-delta * 2);
    this.current.x +=
      ((active ? this.pointer.x : 0) - this.current.x) * (still ? 1 : weight);
    this.current.y +=
      ((active ? this.pointer.y : 0) - this.current.y) * (still ? 1 : weight);
    // Wide parallax swing: near nodes shear hard against the far arc.
    this.camera.position.x = this.current.x * 0.24;
    this.camera.position.y = this.current.y * 0.18;
    this.camera.lookAt(0, 0.08, 0);
  }
}
