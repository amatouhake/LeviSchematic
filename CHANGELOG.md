# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Restored the client build for LeviLamina Client 26.51.3 / Minecraft Bedrock 1.26.51 (`xmake.lua`, `tooth.json`).
- Ghost projection blocks are now injected from the chunk builder's border pass (`RenderChunkBuilder::_sortBlocks` is no longer exported by the game) and are drawn per sub-chunk; projected blocks above the terrain are drawn by the topmost renderable sub-chunk of their column.
- Selection wireframes are drawn from `LevelRendererCamera::renderBlockEntities` and stick-based `pos2` picking hooks `GameMode::useItemOn`.
- Block actor projection and verifier code use the component-based `BlockActor` API.

### Fixed

- Absolute coordinates given to `/schem load|origin|block|pos1|pos2` were treated as local offsets.
- Mismatched full (simple opaque) blocks were not tinted by the verifier.
