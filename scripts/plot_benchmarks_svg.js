#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..");
const SUMMARY_PATH = path.join(
  ROOT,
  ".artifacts",
  "summary_latest-20260909.json"
);
const OUT_DIR = path.join(ROOT, "docs", "benchmark", "charts", "io_uring");

const BNIO = "#2563eb";
const ASIO = "#f59e0b";
const INK = "#111827";
const MUTED = "#6b7280";
const GRID = "#e5e7eb";
const AXIS = "#9ca3af";
const FONT =
  'font-family="DejaVu Sans, Helvetica Neue, Helvetica, Arial, sans-serif"';

const summary = JSON.parse(fs.readFileSync(SUMMARY_PATH, "utf8"));
const throughputRows = summary.throughput_rows;
const throughputRatios = summary.throughput_ratios;
const timerRows = summary.timer_rows;
const timerLifecycleRatios = summary.timer_lifecycle_ratios;
const timerWaitsRatios = summary.timer_waits_ratios;

fs.mkdirSync(OUT_DIR, { recursive: true });

function escapeXml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&apos;");
}

function text(x, y, value, options = {}) {
  const {
    size = 12,
    fill = "#4b5563",
    anchor = "middle",
    bold = false,
    opacity = 1,
  } = options;
  return `<text x="${x}" y="${y}" text-anchor="${anchor}"` +
    ` dominant-baseline="middle" ${FONT} font-size="${size}"` +
    ` fill="${fill}" font-weight="${bold ? "bold" : "normal"}"` +
    ` opacity="${opacity}">${escapeXml(value)}</text>`;
}

function rect(x, y, width, height, fill, options = {}) {
  const { rx = 0, opacity = 1 } = options;
  return `<rect x="${x}" y="${y}" width="${width}" height="${height}"` +
    ` fill="${fill}" rx="${rx}" opacity="${opacity}"></rect>`;
}

function line(x1, y1, x2, y2, options = {}) {
  const { stroke = GRID, width = 1, dash = "" } = options;
  return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"` +
    ` stroke="${stroke}" stroke-width="${width}"` +
    `${dash ? ` stroke-dasharray="${dash}"` : ""}></line>`;
}

function circle(cx, cy, r, fill, stroke = "") {
  return `<circle cx="${cx}" cy="${cy}" r="${r}" fill="${fill}"` +
    `${stroke ? ` stroke="${stroke}" stroke-width="1.5"` : ""}></circle>`;
}

function svgDoc(width, height, body) {
  return `<svg width="${width}" height="${height}"` +
    ` viewBox="0 0 ${width} ${height}"` +
    ` xmlns="http://www.w3.org/2000/svg" version="1.1">` +
    `<rect width="${width}" height="${height}" fill="#ffffff"></rect>` +
    body +
    "</svg>\n";
}

function titleBlock(title, subtitle, width) {
  return (
    text(width / 2, 24, title, {
      size: 18,
      fill: INK,
      bold: true,
    }) +
    text(width / 2, 49, subtitle, { size: 12, fill: MUTED })
  );
}

function legend(x, y) {
  return (
    rect(x, y - 7, 20, 14, BNIO, { rx: 3 }) +
    text(x + 27, y, "bnio", { anchor: "start", fill: INK }) +
    rect(x + 72, y - 7, 20, 14, ASIO, { rx: 3 }) +
    text(x + 99, y, "asio", { anchor: "start", fill: INK })
  );
}

function niceMax(values, divisions = 4) {
  const raw = Math.max(...values);
  if (!raw || raw <= 0) return 1;
  const exponent = Math.floor(Math.log10(raw));
  const power = 10 ** exponent;
  const fraction = raw / power;
  const step = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
  const nice = Math.ceil(raw / (step * power)) * step * power;
  return nice * (1 + 0.05 / divisions);
}

function fmtAxis(value) {
  if (value >= 1_000_000) {
    return `${(value / 1_000_000).toFixed(1)}M`;
  }
  if (value >= 1_000) {
    return `${(value / 1_000).toFixed(1)}k`;
  }
  if (value >= 10) {
    return `${Math.round(value)}`;
  }
  return value.toFixed(2).replace(/\.?0+$/, "");
}

function messageLabel(size) {
  return { 64: "64 B", 1024: "1 KB", 4096: "4 KB", 65536: "64 KB" }[size] ||
    `${size} B`;
}

function throughputRow(server, workers, connections, messageSize) {
  return throughputRows.find(
    (row) =>
      row.server === server &&
      row.workers === workers &&
      row.connections === connections &&
      row.message_size === messageSize
  );
}

function timerRow(server, timers, rounds) {
  return timerRows.find(
    (row) =>
      row.server === server &&
      row.timers === timers &&
      row.rounds === rounds
  );
}

function drawAxes(
  x0,
  y0,
  x1,
  y1,
  maxValue,
  xLabels,
  xLabel,
  labelMode = "bar"
) {
  let body = "";
  const ticks = [0, 0.25, 0.5, 0.75, 1];
  for (const tick of ticks) {
    const y = y1 - (y1 - y0) * tick;
    body += line(x0, y, x1, y, { stroke: GRID });
    body += text(
      x0 - 8,
      y,
      fmtAxis(maxValue * tick),
      { anchor: "end", size: 11, fill: "#4b5563" }
    );
  }
  body += line(x0, y1, x1, y1, { stroke: AXIS, width: 1.2 });
  body += line(x0, y1, x0, y0, { stroke: AXIS, width: 1.2 });
  xLabels.forEach((label, index) => {
    const x =
      labelMode === "line"
        ? x0 + ((x1 - x0) * index) / Math.max(xLabels.length - 1, 1)
        : x0 + ((x1 - x0) * (index + 0.5)) / xLabels.length;
    body += text(x, y1 + 18, label, { size: 11, fill: "#4b5563" });
    body += line(x, y1, x, y1 + 5, { stroke: AXIS });
  });
  body += text((x0 + x1) / 2, y1 + 42, xLabel, {
    size: 12,
    fill: INK,
  });
  return body;
}

function groupedBarChart({
  width = 960,
  height = 540,
  xLabels,
  xLabel,
  title,
  subtitle,
  bnioValues,
  asioValues,
  ratioLabels,
  yLabel = "requests/s",
  filename,
}) {
  const x0 = 90;
  const x1 = 920;
  const y0 = 100;
  const y1 = 470;
  const maxValue = niceMax([...bnioValues, ...asioValues]);
  let body = titleBlock(title, subtitle, width) + legend(650, 68);
  body += drawAxes(x0, y0, x1, y1, maxValue, xLabels, xLabel);
  body += text(18, y0 - 16, yLabel, {
    size: 10,
    fill: INK,
    anchor: "start",
  });

  const slot = (x1 - x0) / xLabels.length;
  const barWidth = slot * 0.28;
  for (let index = 0; index < xLabels.length; index += 1) {
    const center = x0 + slot * (index + 0.5);
    const bnioY = y1 - (bnioValues[index] / maxValue) * (y1 - y0);
    const asioY = y1 - (asioValues[index] / maxValue) * (y1 - y0);
    body += rect(
      center - barWidth - 2,
      bnioY,
      barWidth,
      y1 - bnioY,
      BNIO,
      { rx: 3 }
    );
    body += rect(
      center + 2,
      asioY,
      barWidth,
      y1 - asioY,
      ASIO,
      { rx: 3 }
    );
    if (ratioLabels?.[index]) {
      body += text(
        center,
        Math.min(bnioY, asioY) - 14,
        ratioLabels[index],
        { size: 12, fill: "#374151", bold: true }
      );
    }
  }
  fs.writeFileSync(path.join(OUT_DIR, filename), svgDoc(width, height, body));
}

function lineFacets({
  width = 960,
  height = 630,
  title,
  subtitle,
  panels,
  xLabels,
  xLabel,
  filename,
}) {
  let body = titleBlock(title, subtitle, width) + legend(650, 68);
  const panelWidth = 400;
  const panelGap = 55;
  const leftX = 90;
  const rightX = 545;
  const topY = 100;
  const bottomY = 350;
  const panelPositions = panels.map((_, index) => {
    const top = index < 2;
    const x0 = index % 2 === 0 ? leftX : rightX;
    return {
      x0,
      x1: x0 + panelWidth,
      y0: top ? topY : 360,
      y1: top ? 300 : 535,
    };
  });

  panels.forEach((panel, index) => {
    const { x0, x1, y0, y1 } = panelPositions[index];
    const values = panel.bnioValues.concat(panel.asioValues);
    const maxValue = niceMax(values);
    body += text((x0 + x1) / 2, y0 - 14, panel.label, {
      size: 13,
      fill: INK,
      bold: true,
    });
    body += drawAxes(x0, y0, x1, y1, maxValue, xLabels, xLabel, "line");
    const coords = (valuesArray, color) => {
      const points = valuesArray.map((value, pointIndex) => {
        const x = x0 + ((x1 - x0) * pointIndex) / (xLabels.length - 1);
        const y = y1 - (value / maxValue) * (y1 - y0);
        return { x, y };
      });
      const path = points
        .map((point, pointIndex) => `${pointIndex ? "L" : "M"}${point.x} ${point.y}`)
        .join(" ");
      return { path, points, color };
    };

    const bnio = coords(panel.bnioValues, BNIO);
    const asio = coords(panel.asioValues, ASIO);
    body += `<path d="${bnio.path}" fill="none" stroke="${BNIO}" stroke-width="2.5" stroke-linejoin="round"></path>`;
    body += `<path d="${asio.path}" fill="none" stroke="${ASIO}" stroke-width="2.5" stroke-linejoin="round"></path>`;
    for (const point of bnio.points) {
      body += circle(point.x, point.y, 4, "#ffffff", BNIO);
    }
    for (const point of asio.points) {
      body += circle(point.x, point.y, 4, "#ffffff", ASIO);
    }
  });
  fs.writeFileSync(path.join(OUT_DIR, filename), svgDoc(width, height, body));
}

function timerFacets({
  title,
  subtitle,
  metric,
  filename,
}) {
  const width = 960;
  const height = 630;
  const rounds = [100, 500, 1000];
  const timers = [256, 1024, 4096, 16384];
  let body = titleBlock(title, subtitle, width) + legend(630, 68);
  const panelWidth = 235;
  const x0s = [90, 355, 620];
  const y0 = 105;
  const y1 = 465;

  rounds.forEach((round, index) => {
    const x0 = x0s[index];
    const x1 = x0 + panelWidth;
    const bnioValues = timers.map((timersValue) =>
      timerRow("bnio", timersValue, round)[`${metric}_per_s`]
    );
    const asioValues = timers.map((timersValue) =>
      timerRow("asio", timersValue, round)[`${metric}_per_s`]
    );
    const maxValue = niceMax([...bnioValues, ...asioValues]);
    body += drawAxes(
      x0,
      y0,
      x1,
      y1,
      maxValue,
      timers.map((value) => (value >= 1024 ? `${value / 1024}k` : `${value}`)),
      "live timers",
      "line"
    );
    body += text((x0 + x1) / 2, 86, `rounds=${round}`, {
      size: 13,
      fill: INK,
      bold: true,
    });
    const mapCoords = (valuesArray, color) => {
      const points = valuesArray.map((value, pointIndex) => {
        const x = x0 + ((x1 - x0) * pointIndex) / (timers.length - 1);
        const y = y1 - (value / maxValue) * (y1 - y0);
        return { x, y };
      });
      return {
        path: points
          .map((point, pointIndex) => `${pointIndex ? "L" : "M"}${point.x} ${point.y}`)
          .join(" "),
        points,
        color,
      };
    };
    const bnio = mapCoords(bnioValues, BNIO);
    const asio = mapCoords(asioValues, ASIO);
    body += `<path d="${bnio.path}" fill="none" stroke="${BNIO}" stroke-width="2.5" stroke-linejoin="round"></path>`;
    body += `<path d="${asio.path}" fill="none" stroke="${ASIO}" stroke-width="2.5" stroke-linejoin="round"></path>`;
    for (const point of bnio.points) {
      body += circle(point.x, point.y, 4, "#ffffff", BNIO);
    }
    for (const point of asio.points) {
      body += circle(point.x, point.y, 4, "#ffffff", ASIO);
    }
  });
  fs.writeFileSync(path.join(OUT_DIR, filename), svgDoc(width, height, body));
}

function heatColor(value, min = -30, max = 30) {
  const t = Math.max(0, Math.min(1, (value - min) / (max - min)));
  const red = [239, 68, 68];
  const blue = [37, 99, 235];
  const white = [255, 255, 255];
  const midpoint = 0.5;
  const from = t < midpoint ? red : white;
  const to = t < midpoint ? white : blue;
  const local = t < midpoint ? t / midpoint : (t - midpoint) / midpoint;
  const color = from.map((channel, index) =>
    Math.round(channel + (to[index] - channel) * local)
  );
  return `rgb(${color.join(",")})`;
}

function heatmap({
  width = 960,
  height = 540,
  title,
  subtitle,
  rows,
  columns,
  values,
  rowLabels,
  columnLabels,
  xLabel = "connections",
  yLabel = "message size",
  filename,
}) {
  const x0 = 110;
  const x1 = 820;
  const y0 = 110;
  const y1 = 450;
  const cellW = (x1 - x0) / columns.length;
  const cellH = (y1 - y0) / rows.length;
  let body = titleBlock(title, subtitle, width);

  for (let row = 0; row < rows.length; row += 1) {
    for (let column = 0; column < columns.length; column += 1) {
      const value = values[row][column];
      const x = x0 + column * cellW;
      const y = y0 + row * cellH;
      body += rect(x, y, cellW, cellH, heatColor(value), { rx: 3 });
      body += text(
        x + cellW / 2,
        y + cellH / 2,
        `${value > 0 ? "+" : ""}${value.toFixed(1)}%`,
        { size: 11, fill: INK, bold: true }
      );
    }
  }

  body += line(x0, y1, x1, y1, { stroke: AXIS, width: 1.2 });
  body += line(x0, y0, x0, y1, { stroke: AXIS, width: 1.2 });
  columnLabels.forEach((label, index) => {
    const x = x0 + cellW * (index + 0.5);
    body += text(x, y1 + 18, label, { size: 11, fill: "#4b5563" });
    body += line(x, y1, x, y1 + 5, { stroke: AXIS });
  });
  rowLabels.forEach((label, index) => {
    body += text(x0 - 10, y0 + cellH * (index + 0.5), label, {
      size: 11,
      fill: "#4b5563",
      anchor: "end",
    });
  });
  body += text((x0 + x1) / 2, y1 + 42, xLabel, {
    size: 12,
    fill: INK,
  });
  body += text(4, (y0 + y1) / 2, yLabel, {
    size: 12,
    fill: INK,
    anchor: "start",
  });

  // Small legend/colorbar on the right.
  const legendX = 860;
  const legendY0 = 130;
  const legendY1 = 430;
  body += `<defs><linearGradient id="heat-gradient" x1="0" y1="0" x2="0" y2="1">` +
    `<stop offset="0%" stop-color="#2563eb"></stop>` +
    `<stop offset="50%" stop-color="#ffffff"></stop>` +
    `<stop offset="100%" stop-color="#ef4444"></stop>` +
    `</linearGradient></defs>`;
  body += rect(legendX, legendY0, 22, legendY1 - legendY0, "url(#heat-gradient)", {
    rx: 3,
  });
  body += text(legendX + 31, legendY0, "bnio faster", {
    size: 11,
    fill: INK,
    anchor: "start",
  });
  body += text(legendX + 31, legendY1, "asio faster", {
    size: 11,
    fill: INK,
    anchor: "start",
  });
  fs.writeFileSync(path.join(OUT_DIR, filename), svgDoc(width, height, body));
}

function overviewChart() {
  const messageSizes = [64, 1024, 4096, 65536];
  const bnioValues = messageSizes.map((size) =>
    throughputRow("bnio", 4, 256, size).rate
  );
  const asioValues = messageSizes.map((size) =>
    throughputRow("asio", 4, 256, size).rate
  );
  const ratioLabels = messageSizes.map(
    (size) => `${(bnioValues[messageSizes.indexOf(size)] / asioValues[messageSizes.indexOf(size)]).toFixed(2)}×`
  );
  groupedBarChart({
    xLabels: messageSizes.map(messageLabel),
    xLabel: "message size",
    title: "TCP Echo Throughput Overview (io_uring)",
    subtitle: "workers=4, connections=256, 3-iteration mean",
    bnioValues,
    asioValues,
    ratioLabels,
    filename: "overview_bars.svg",
  });
}

function throughputFacets({ xKey, filters, filename, title, subtitle, xLabel }) {
  const dimensions = xKey === "workers" ? [1, 2, 4, 8] : [64, 256, 1024];
  const panels = [64, 1024, 4096, 65536].map((messageSize) => ({
    label: messageLabel(messageSize),
    bnioValues: dimensions.map((value) => {
      const row = throughputRows.find(
        (candidate) =>
          candidate.server === "bnio" &&
          candidate.message_size === messageSize &&
          candidate[xKey] === value &&
          Object.entries(filters).every(([key, expected]) => candidate[key] === expected)
      );
      return row.rate;
    }),
    asioValues: dimensions.map((value) => {
      const row = throughputRows.find(
        (candidate) =>
          candidate.server === "asio" &&
          candidate.message_size === messageSize &&
          candidate[xKey] === value &&
          Object.entries(filters).every(([key, expected]) => candidate[key] === expected)
      );
      return row.rate;
    }),
  }));
  lineFacets({
    title,
    subtitle,
    panels,
    xLabels: dimensions.map(String),
    xLabel,
    filename,
  });
}

function ratioHeatmap({ rows, columns, values, filename, title, subtitle, columnLabel }) {
  heatmap({
    width: 960,
    height: 540,
    title,
    subtitle,
    rows,
    columns,
    values,
    rowLabels: rows.map(messageLabel),
    columnLabels: columns.map(String),
    filename,
  });
}

function timerOverview({ metric, label, filename, yLabel = "operations/s" }) {
  const timers = [256, 1024, 4096, 16384];
  const bnioValues = timers.map((value) => timerRow("bnio", value, 500)[`${metric}_per_s`]);
  const asioValues = timers.map((value) => timerRow("asio", value, 500)[`${metric}_per_s`]);
  const ratioLabels = timers.map((value) => {
    const bnio = timerRow("bnio", value, 500)[`${metric}_per_s`];
    const asio = timerRow("asio", value, 500)[`${metric}_per_s`];
    return `${(bnio / asio).toFixed(2)}×`;
  });
  groupedBarChart({
    xLabels: timers.map(String),
    xLabel: "live timers",
    title: `Timer ${label} Throughput Overview`,
    subtitle: "rounds=500, 3-iteration mean",
    bnioValues,
    asioValues,
    ratioLabels,
    yLabel,
    filename,
  });
}

function timerRatioHeatmap({ metric, title, filename }) {
  const ratios = metric === "lifecycle" ? timerLifecycleRatios : timerWaitsRatios;
  const timers = [256, 1024, 4096, 16384];
  const rounds = [100, 500, 1000];
  const values = rounds.map((round) =>
    timers.map((timerCount) => {
      const row = ratios.find(
        (candidate) => candidate.timers === timerCount && candidate.rounds === round
      );
      return (row.ratio - 1) * 100;
    })
  );
  heatmap({
    width: 960,
    height: 540,
    title,
    subtitle: "positive (blue) = bnio faster; negative (red) = asio faster",
    rows: [1, 2, 3],
    columns: timers,
    values,
    rowLabels: rounds.map(String),
    columnLabels: timers.map(String),
    xLabel: "live timers",
    yLabel: "update rounds",
    filename,
  });
}

function main() {
  overviewChart();

  throughputFacets({
    xKey: "connections",
    filters: { workers: 4 },
    filename: "throughput_vs_connections.svg",
    title: "Throughput vs Connections",
    subtitle: "workers=4, faceted by message size",
    xLabel: "connections",
  });
  throughputFacets({
    xKey: "workers",
    filters: { connections: 256 },
    filename: "throughput_vs_workers.svg",
    title: "Throughput vs Worker Threads",
    subtitle: "connections=256, faceted by message size",
    xLabel: "workers",
  });

  const throughputHeat = (workers) =>
    throughputRatios
      .filter((row) => row.workers === workers)
      .sort((a, b) => a.message_size - b.message_size || a.connections - b.connections);
  const connectionRatios = throughputHeat(4);
  const connectionValues = [64, 1024, 4096, 65536].map((messageSize) =>
    [64, 256, 1024].map(
      (connections) =>
        (connectionRatios.find(
          (row) => row.message_size === messageSize && row.connections === connections
        ).ratio - 1) * 100
    )
  );
  heatmap({
    title: "bnio/asio Throughput Ratio Heatmap (workers=4)",
    subtitle: "positive (blue) = bnio faster; negative (red) = asio faster",
    rows: [64, 1024, 4096, 65536],
    columns: [64, 256, 1024],
    values: connectionValues,
    rowLabels: [64, 1024, 4096, 65536].map(messageLabel),
    columnLabels: ["64", "256", "1024"],
    xLabel: "connections",
    yLabel: "message size",
    filename: "heatmap_ratio.svg",
  });

  const workerRatios = throughputRatios
    .filter((row) => row.connections === 256)
    .sort((a, b) => a.message_size - b.message_size || a.workers - b.workers);
  const workerValues = [64, 1024, 4096, 65536].map((messageSize) =>
    [1, 2, 4, 8].map(
      (workers) =>
        (workerRatios.find(
          (row) => row.message_size === messageSize && row.workers === workers
        ).ratio - 1) * 100
    )
  );
  heatmap({
    title: "bnio/asio Worker Scaling Ratio Heatmap (connections=256)",
    subtitle: "positive (blue) = bnio faster; negative (red) = asio faster",
    rows: [64, 1024, 4096, 65536],
    columns: [1, 2, 4, 8],
    values: workerValues,
    rowLabels: [64, 1024, 4096, 65536].map(messageLabel),
    columnLabels: ["1", "2", "4", "8"],
    xLabel: "workers",
    yLabel: "message size",
    filename: "worker_scaling_heatmap.svg",
  });

  timerOverview({
    metric: "lifecycle",
    label: "Lifecycle",
    yLabel: "operations/s",
    filename: "timer_lifecycle_overview.svg",
  });
  timerOverview({
    metric: "waits",
    label: "Active Waits",
    yLabel: "operations/s",
    filename: "timer_waits_overview.svg",
  });
  timerFacets({
    metric: "lifecycle",
    title: "Timer Lifecycle Throughput vs Timer Count",
    subtitle: "faceted by update rounds",
    filename: "timer_lifecycle_vs_timers.svg",
  });
  timerRatioHeatmap({
    metric: "lifecycle",
    title: "bnio/asio Lifecycle Ratio Heatmap",
    filename: "timer_lifecycle_heatmap.svg",
  });
  timerRatioHeatmap({
    metric: "waits",
    title: "bnio/asio Active Waits Ratio Heatmap",
    filename: "timer_waits_heatmap.svg",
  });
}

main();
