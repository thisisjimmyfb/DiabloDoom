"""Gymnasium environment for DiabloDoom running under -rlenv.

    import gymnasium as gym
    import env
    env = gym.make("DiabloDoom-v0")

See agent/training/SPEC.md for the design and README.md for usage.
"""

from gymnasium.envs.registration import register

from .engine import Engine, EngineConfig, EngineError, cleanup_stale
from .env import (ACTIONS, CONSULT, DiabloDoomEnv, EnvConfig, RewardConfig,
                  null_consult, standin_consult)

register(id="DiabloDoom-v0", entry_point="env.env:DiabloDoomEnv")

__all__ = ["ACTIONS", "CONSULT", "DiabloDoomEnv", "EnvConfig", "RewardConfig",
           "Engine", "EngineConfig", "EngineError", "cleanup_stale",
           "null_consult", "standin_consult"]
