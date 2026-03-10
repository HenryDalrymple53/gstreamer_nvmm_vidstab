import cv2
import numpy as np
import argparse
import sys
from collections import deque

# Lucas-Kanade optical flow parameters
LK_PARAMS = dict(
    winSize=(21, 21),
    maxLevel=4,
    criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 30, 0.01)
)

# Harris corner detection parameters
HARRIS_BLOCK_SIZE = 2
HARRIS_KSIZE      = 3
HARRIS_K          = 0.04
HARRIS_THRESHOLD  = 0.01
HARRIS_MAX_PTS    = 1000
HARRIS_MIN_DIST   = 7

# Stabilization parameters
SMOOTHING_WINDOW  = 5   # frames (~150ms at 30fps)
REDETECT_INTERVAL = 30


def harris_detect(gray):
    """Detect Harris corners and return them as (N,1,2) float32 array."""
    dst = cv2.cornerHarris(gray, HARRIS_BLOCK_SIZE, HARRIS_KSIZE, HARRIS_K)
    dst = cv2.dilate(dst, None)
    threshold = HARRIS_THRESHOLD * dst.max()
    ys, xs = np.where(dst > threshold)

    if len(xs) == 0:
        return None

    pts = np.column_stack((xs, ys)).astype(np.float32)

    selected = []
    occupied = np.zeros(gray.shape, dtype=bool)
    for x, y in pts:
        xi, yi = int(x), int(y)
        r = HARRIS_MIN_DIST
        if not occupied[max(0,yi-r):yi+r+1, max(0,xi-r):xi+r+1].any():
            selected.append([x, y])
            occupied[max(0,yi-r):yi+r+1, max(0,xi-r):xi+r+1] = True
        if len(selected) >= HARRIS_MAX_PTS:
            break

    if not selected:
        return None
    return np.array(selected, dtype=np.float32).reshape(-1, 1, 2)


def affine_to_components(A):
    """Decompose 2x3 affine matrix into (dx, dy, angle)."""
    dx    = A[0, 2]
    dy    = A[1, 2]
    angle = np.arctan2(A[1, 0], A[0, 0])
    return dx, dy, angle


def components_to_affine(dx, dy, angle):
    """Build 2x3 affine matrix from (dx, dy, angle)."""
    c = np.cos(angle)
    s = np.sin(angle)
    return np.array([
        [c, -s, dx],
        [s,  c, dy]
    ], dtype=np.float64)


def draw_tracks(frame, good_new, good_old, mask):
    for new, old in zip(good_new, good_old):
        a, b = new.ravel().astype(int)
        c, d = old.ravel().astype(int)
        mask  = cv2.line(mask, (a, b), (c, d), (0, 255, 0), 1)
        frame = cv2.circle(frame, (a, b), 4, (0, 0, 255), -1)
    return cv2.add(frame, mask)


def process_video(input_path: str, output_path: str):
    cap = cv2.VideoCapture(input_path)
    if not cap.isOpened():
        print(f"Error: cannot open '{input_path}'")
        sys.exit(1)

    width  = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps    = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total  = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out    = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

    ret, first_frame = cap.read()
    if not ret:
        print("Error: could not read first frame")
        sys.exit(1)

    prev_gray = cv2.cvtColor(first_frame, cv2.COLOR_BGR2GRAY)
    prev_pts  = harris_detect(prev_gray)
    mask      = np.zeros_like(first_frame)

    out.write(first_frame)

    # Trajectory: cumulative sum of per-frame transforms (dx, dy, angle)
    cum_dx    = 0.0
    cum_dy    = 0.0
    cum_angle = 0.0

    # Circular buffer of last SMOOTHING_WINDOW cumulative transforms
    traj_buf = deque(maxlen=SMOOTHING_WINDOW)
    traj_buf.append((cum_dx, cum_dy, cum_angle))

    frame_idx = 1
    while True:
        ret, frame = cap.read()
        if not ret:
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # --- Optical flow ---
        delta_dx, delta_dy, delta_angle = 0.0, 0.0, 0.0

        if prev_pts is not None and len(prev_pts) > 0:
            next_pts, status, _ = cv2.calcOpticalFlowPyrLK(
                prev_gray, gray, prev_pts, None, **LK_PARAMS
            )

            if next_pts is not None and status is not None:
                good_new = next_pts[status == 1]
                good_old = prev_pts[status == 1]

                if len(good_new) >= 4:
                    # Estimate rigid transform (translation + rotation, no shear)
                    A, inliers = cv2.estimateAffinePartial2D(
                        good_old, good_new,
                        method=cv2.RANSAC,
                        ransacReprojThreshold=3.0
                    )
                    if A is not None:
                        delta_dx, delta_dy, delta_angle = affine_to_components(A)

                frame = draw_tracks(frame, good_new, good_old, mask)
                prev_pts = good_new.reshape(-1, 1, 2)
            else:
                prev_pts = None
        else:
            prev_pts = None

        # Re-detect features periodically or when too few remain
        if prev_pts is None or len(prev_pts) < 50 or frame_idx % REDETECT_INTERVAL == 0:
            mask     = np.zeros_like(frame)
            prev_pts = harris_detect(gray)

        # --- Accumulate trajectory ---
        cum_dx    += delta_dx
        cum_dy    += delta_dy
        cum_angle += delta_angle
        traj_buf.append((cum_dx, cum_dy, cum_angle))

        # --- Smooth trajectory (mean of window) ---
        buf_arr      = np.array(traj_buf)
        smooth_dx    = buf_arr[:, 0].mean()
        smooth_dy    = buf_arr[:, 1].mean()
        smooth_angle = buf_arr[:, 2].mean()

        # --- Correction = smoothed - raw ---
        corr_dx    = smooth_dx    - cum_dx
        corr_dy    = smooth_dy    - cum_dy
        corr_angle = smooth_angle - cum_angle

        # --- Build correction affine and warp ---
        M = components_to_affine(corr_dx, corr_dy, corr_angle)
        stabilized = cv2.warpAffine(
            frame, M, (width, height),
            flags=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=(0, 0, 0)   # black fill
        )

        out.write(stabilized)
        prev_gray = gray
        frame_idx += 1

        if frame_idx % 30 == 0:
            pct = (frame_idx / total * 100) if total > 0 else 0
            print(f"\r  {frame_idx}/{total} frames ({pct:.1f}%)", end="", flush=True)

    print()
    cap.release()
    out.release()
    print(f"Done — saved to '{output_path}'")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stabilise video using LK optical flow")
    parser.add_argument("input",  help="Path to input video")
    parser.add_argument("output", help="Path to output video (mp4)")
    args = parser.parse_args()

    process_video(args.input, args.output)