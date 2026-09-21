export const strategies = [
  {
    identity: {
      id: "builtin.sma-long",
      version: "1.0.0",
      digest: "a".repeat(64),
    },
    name: "双均线",
    description: "快均线高于慢均线时做多，按慢均线周期预热。",
    parameters: [
      {
        key: "fast",
        label: "快均线周期",
        type: "integer",
        minimum: 1,
        maximum: 250,
        default: 5,
      },
      {
        key: "slow",
        label: "慢均线周期",
        type: "integer",
        minimum: 2,
        maximum: 500,
        default: 20,
      },
    ],
  },
  {
    identity: {
      id: "builtin.momentum-long",
      version: "1.0.0",
      digest: "b".repeat(64),
    },
    name: "收盘动量",
    description: "回看周期加一根日线预热。",
    parameters: [
      {
        key: "lookback",
        label: "回看周期",
        type: "integer",
        minimum: 1,
        maximum: 499,
        default: 10,
      },
    ],
  },
];
