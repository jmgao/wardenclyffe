class WebGLRenderer {
  #canvas = null;
  #ctx = null;

  static vertexShaderSource = `
    attribute vec2 xy;
    varying highp vec2 uv;
    void main(void) {
      gl_Position = vec4(xy, 0.0, 1.0);
      // Map vertex coordinates (-1 to +1) to UV coordinates (0 to 1).
      // UV coordinates are Y-flipped relative to vertex coordinates.
      uv = vec2((1.0 + xy.x) / 2.0, (1.0 - xy.y) / 2.0);
    }
  `;

  static fragmentShaderSource = `
    varying highp vec2 uv;
    uniform sampler2D texture;
    void main(void) {
      gl_FragColor = texture2D(texture, uv);
    }
  `;

  constructor(canvas) {
    this.#canvas = canvas;
    const gl = this.#ctx = canvas.getContext("webgl");

    const vertexShader = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vertexShader, WebGLRenderer.vertexShaderSource);
    gl.compileShader(vertexShader);
    if (!gl.getShaderParameter(vertexShader, gl.COMPILE_STATUS)) {
      throw gl.getShaderInfoLog(vertexShader);
    }

    const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fragmentShader, WebGLRenderer.fragmentShaderSource);
    gl.compileShader(fragmentShader);
    if (!gl.getShaderParameter(fragmentShader, gl.COMPILE_STATUS)) {
      throw gl.getShaderInfoLog(fragmentShader);
    }

    const shaderProgram = gl.createProgram();
    gl.attachShader(shaderProgram, vertexShader);
    gl.attachShader(shaderProgram, fragmentShader);
    gl.linkProgram(shaderProgram);
    if (!gl.getProgramParameter(shaderProgram, gl.LINK_STATUS)) {
      throw gl.getProgramInfoLog(shaderProgram);
    }
    gl.useProgram(shaderProgram);

    // Vertex coordinates, clockwise from bottom-left.
    const vertexBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
      -1.0, -1.0,
      -1.0, +1.0,
      +1.0, +1.0,
      +1.0, -1.0
    ]), gl.STATIC_DRAW);

    const xyLocation = gl.getAttribLocation(shaderProgram, "xy");
    gl.vertexAttribPointer(xyLocation, 2, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(xyLocation);

    // Create one texture to upload frames to.
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  draw(frame) {
    this.#canvas.width = frame.displayWidth;
    this.#canvas.height = frame.displayHeight;

    const gl = this.#ctx;

    // Upload the frame.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
    frame.close();

    // Configure and clear the drawing area.
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.clearColor(1.0, 0.0, 0.0, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // Draw the frame.
    gl.drawArrays(gl.TRIANGLE_FAN, 0, 4);
  }
};

var pendingStatus = {};
function setStatus(type, message) {
  pendingStatus[type] = message;
}

function postStatus() {
  self.postMessage(pendingStatus);
  pendingStatus = {};
}

// Rendering. Drawing is limited to once per animation frame.
let decoder = null;
let renderer = null;

let pendingFrames = [];

let startTime = null;
let decodeFrameCount = 0;
let renderFrameCount = 0;

function enqueueFrame(frame) {
  pendingFrames.push(frame);
}

function renderFrame() {
  if (pendingFrames.length != 0) {
    ++renderFrameCount;
    const pendingFrame = pendingFrames.shift();
    renderer.draw(pendingFrame);
  }

  requestAnimationFrame(renderFrame);
}

function updateStats() {
  // Update statistics once a second.
  const now = performance.now();
  if (startTime == null) {
    startTime = now;
  } else {
    const elapsed = (now - startTime) / 1000;
    if (elapsed > 1.0) {
      const renderFps = renderFrameCount / elapsed;
      const decodeFps = decodeFrameCount / elapsed;

      renderFrameCount = 0;
      decodeFrameCount = 0;
      startTime = now;

      setStatus("render", `${renderFps.toFixed(0)} fps`);
      setStatus("decode", `${decodeFps.toFixed(0)} fps`);
      setStatus("renderqueue", `${pendingFrames.length} frame(s)`);
      postStatus();
    }
  }
}

async function receiveFrame(frame) {
  const decoder = new ImageDecoder({
    type: "image/jpeg",
    data: frame.data.data,
  });

  // TODO: Support progressive decoding
  const decoded = await decoder.decode();
  ++decodeFrameCount;
  updateStats();

  // Schedule the frame to be rendered.
  enqueueFrame(decoded.image);
}

function start({host, canvas}) {
  renderer = new WebGLRenderer(canvas);
  decoder = new VideoDecoder({
    output(frame) {
    },
    error(e) {
      console.log("error", e);
      setStatus("decode", e);
    }
  });

  self.addEventListener("message", receiveFrame);
  requestAnimationFrame(renderFrame);
}

self.addEventListener("message", message => start(message.data), {once: true});
