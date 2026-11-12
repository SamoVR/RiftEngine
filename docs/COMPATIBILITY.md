# How much of Roblox's API RIFT has

This compares RIFT with what Roblox scripts commonly use, listed by hand in `tools/roblox_api.txt`. A running game was asked for each name. It is not all of Roblox's API, and a name being here means it exists, not that it behaves identically.

Of what is listed: 115 of 134 classes and services, and 1206 of 1457 names (82%).

## Services

- **Workspace**: all 19
- **Players**: 11 of 12. Not yet: GetHumanoidDescriptionFromUserId
- **RunService**: all 12
- **UserInputService**: all 24
- **ContextActionService**: all 8
- **TweenService**: all 2
- **Debris**: all 1
- **CollectionService**: all 7
- **ReplicatedStorage**: all 1
- **ReplicatedFirst**: all 1
- **ServerStorage**: all 1
- **ServerScriptService**: all 1
- **StarterGui**: all 7
- **StarterPack**: all 1
- **StarterPlayer**: all 14
- **Lighting**: all 20
- **SoundService**: 5 of 8. Not yet: PlayLocalSound, SetListener, GetListener
- **Teams**: all 1
- **HttpService**: all 8
- **DataStoreService**: all 4
- **TextChatService**: 5 of 8. Not yet: ChatVersion, ChatInputBarConfiguration, OnIncomingMessage
- **PathfindingService**: all 2
- **PhysicsService**: answers, but does nothing (see below)
- **MarketplaceService**: answers, but does nothing (see below)
- **BadgeService**: answers, but does nothing (see below)
- **TeleportService**: answers, but does nothing (see below)
- **GuiService**: answers, but does nothing (see below)
- **MessagingService**: answers, but does nothing (see below)
- **MemoryStoreService**: answers, but does nothing (see below)
- **LocalizationService**: answers, but does nothing (see below)
- **AssetService**: answers, but does nothing (see below)
- **Stats**: answers, but does nothing (see below)
- **GroupService**: answers, but does nothing (see below)
- **SocialService**: answers, but does nothing (see below)
- **ContentProvider**: answers, but does nothing (see below)
- **VRService**: answers, but does nothing (see below)
- **HapticService**: answers, but does nothing (see below)
- **Chat**: all 3

## Classes

- **Instance**: all 38
- **Part**: all 47
- **MeshPart**: all 5
- **Model**: all 11
- **Humanoid**: 48 of 53. Not yet: ApplyDescription, GetAppliedDescription, AddAccessory, Climbing, Touched
- **Player**: 31 of 33. Not yet: CharacterAppearanceLoaded, Idled
- **Camera**: all 15
- **Tool**: all 17
- **Sound**: all 25
- **Animator**: 3 of 4. Not yet: ApplyJointVelocities
- **Animation**: all 1
- **Attachment**: all 9
- **ParticleEmitter**: all 30
- **Beam**: 19 of 20. Not yet: SetTextureOffset
- **Trail**: 16 of 17. Not yet: Clear
- **PointLight**: all 5
- **SpotLight**: all 7
- **SurfaceLight**: not yet
- **Decal**: all 5
- **Texture**: not yet
- **Highlight**: all 7
- **ProximityPrompt**: all 15
- **ClickDetector**: all 6
- **Seat**: all 3
- **VehicleSeat**: 7 of 9. Not yet: TurnSpeed, HeadsUpDisplay
- **RemoteEvent**: all 5
- **RemoteFunction**: all 4
- **BindableEvent**: all 2
- **BindableFunction**: all 2
- **ScreenGui**: all 7
- **Frame**: 22 of 27. Not yet: AbsoluteRotation, MouseMoved, InputBegan, InputChanged, InputEnded
- **TextLabel**: all 18
- **TextButton**: all 11
- **TextBox**: 13 of 14. Not yet: ReturnPressedFromOnScreenKeyboard
- **ImageLabel**: all 9
- **ImageButton**: all 6
- **ScrollingFrame**: 8 of 9. Not yet: AbsoluteCanvasSize
- **BillboardGui**: 7 of 10. Not yet: ExtentsOffset, LightInfluence, PlayerToHideFrom
- **SurfaceGui**: 7 of 9. Not yet: LightInfluence, Brightness
- **ViewportFrame**: all 6
- **UIListLayout**: 5 of 10. Not yet: Wraps, AbsoluteContentSize, HorizontalFlex, VerticalFlex, ItemLineAlignment
- **UIGridLayout**: 3 of 11. Not yet: FillDirection, FillDirectionMaxCells, HorizontalAlignment, VerticalAlignment, StartCorner, AbsoluteCellCount, AbsoluteCellSize, AbsoluteContentSize
- **UICorner**: all 1
- **UIStroke**: 4 of 6. Not yet: ApplyStrokeMode, LineJoinMode
- **UIPadding**: all 4
- **UIScale**: all 1
- **UIAspectRatioConstraint**: 1 of 3. Not yet: AspectType, DominantAxis
- **UIGradient**: all 5
- **UISizeConstraint**: all 2
- **UITextSizeConstraint**: not yet
- **UIFlexItem**: not yet
- **UIPageLayout**: 10 of 15. Not yet: EasingDirection, EasingStyle, PageEnter, PageLeave, Stopped
- **UITableLayout**: not yet
- **CanvasGroup**: not yet
- **Motor6D**: all 10
- **Weld**: not yet
- **WeldConstraint**: 3 of 4. Not yet: Active
- **HingeConstraint**: 13 of 14. Not yet: Restitution
- **SpringConstraint**: 6 of 14. Not yet: LimitsEnabled, MinLength, MaxLength, Coils, Radius, Thickness, Visible, Color
- **RopeConstraint**: 4 of 10. Not yet: Restitution, Thickness, Visible, WinchEnabled, WinchTarget, WinchSpeed
- **RodConstraint**: 4 of 9. Not yet: LimitAngle0, LimitAngle1, LimitsEnabled, Thickness, Visible
- **PrismaticConstraint**: all 12
- **BallSocketConstraint**: 2 of 7. Not yet: LimitsEnabled, UpperAngle, TwistLimitsEnabled, TwistLowerAngle, TwistUpperAngle
- **NoCollisionConstraint**: all 3
- **LinearVelocity**: 3 of 6. Not yet: Attachment0, RelativeTo, VelocityConstraintMode
- **AngularVelocity**: 3 of 5. Not yet: Attachment0, RelativeTo
- **VectorForce**: 2 of 5. Not yet: Attachment0, RelativeTo, ApplyAtCenterOfMass
- **Torque**: not yet
- **AlignPosition**: 4 of 9. Not yet: Attachment0, Attachment1, Mode, MaxVelocity, RigidityEnabled
- **AlignOrientation**: not yet
- **BodyVelocity**: not yet
- **BodyPosition**: not yet
- **BodyGyro**: not yet
- **BodyForce**: not yet
- **BodyAngularVelocity**: not yet
- **Fire**: all 6
- **Smoke**: 5 of 6. Not yet: Opacity
- **Sparkles**: all 3
- **Explosion**: all 7
- **SelectionBox**: not yet
- **BoxHandleAdornment**: not yet
- **SphereHandleAdornment**: not yet
- **CylinderHandleAdornment**: not yet
- **Sky**: not yet
- **Atmosphere**: 4 of 6. Not yet: Offset, Decay
- **BloomEffect**: all 4
- **BlurEffect**: all 2
- **ColorCorrectionEffect**: all 5
- **DepthOfFieldEffect**: all 5
- **SunRaysEffect**: all 3
- **Team**: all 5
- **SpawnLocation**: all 5
- **StringValue**: all 2
- **Configuration**: all 1
- **Folder**: all 1
- **Terrain**: 17 of 18. Not yet: FillWedge

## Libraries

- **task**: all 7
- **math**: all 35
- **string**: all 16
- **table**: all 14
- **os**: all 4
- **coroutine**: all 8
- **utf8**: all 9
- **bit32**: all 14

## Types

- **Vector3**: all 8
- **Vector2**: all 5
- **CFrame**: all 11
- **Color3**: all 5
- **UDim**: all 1
- **UDim2**: all 3
- **Ray**: all 1
- **Region3**: all 1
- **TweenInfo**: all 1
- **NumberRange**: all 1
- **NumberSequence**: all 1
- **NumberSequenceKeypoint**: all 1
- **ColorSequence**: all 1
- **ColorSequenceKeypoint**: all 1
- **Rect**: all 1
- **BrickColor**: all 9
- **Random**: all 1
- **RaycastParams**: all 1
- **OverlapParams**: all 1
- **PhysicalProperties**: all 1
- **Font**: all 4
- **DateTime**: all 5
- **Instance**: all 2

## What values offer

- **Vector3**: all 16
- **Vector2**: all 14
- **CFrame**: all 28
- **Color3**: all 6
- **UDim2**: all 5
- **AnimationTrack**: all 19
- **Tween**: all 5
- **RBXScriptSignal**: 3 of 4. Not yet: ConnectParallel
- **RBXScriptConnection**: all 2
- **Mouse**: 14 of 21. Not yet: Target, Origin, TargetFilter, TargetSurface, Idle, KeyDown, KeyUp
- **RaycastResult**: all 5
- **InputObject**: 5 of 6. Not yet: IsModifierKeyDown
- **Path**: all 6

## Other classes

- Has: NumberValue, IntValue, BoolValue, ObjectValue, Vector3Value, Color3Value, Script, LocalScript, Backpack, PlayerGui, HumanoidDescription, TrussPart, SurfaceAppearance, PathfindingModifier, EchoSoundEffect, ReverbSoundEffect, DistortionSoundEffect, EqualizerSoundEffect, TremoloSoundEffect, SoundGroup, ClickDetector, RopeConstraint, IKControl, AnimationController
- Not yet: CFrameValue, BrickColorValue, RayValue, StarterGear, Hat, Accessory, Shirt, Pants, ShirtGraphic, BodyColors, CharacterMesh, SpecialMesh, BlockMesh, CylinderMesh, WedgePart, CornerWedgePart, UnionOperation, NegateOperation, PathfindingLink, Dialog, DialogChoice, Flag, FlagStand, ForceField, PointsService, ChatWindowConfiguration, BubbleChatConfiguration, AudioPlayer, CompressorSoundEffect, ChorusSoundEffect, FlangeSoundEffect, PitchShiftSoundEffect, UnreliableRemoteEvent, Actor, WrapLayer, WrapTarget, LineForce, PlaneConstraint, CylindricalConstraint, UniversalConstraint, TorsionSpringConstraint, Bone

## Accepted, but does nothing yet

So that a game carries on: these can be read and set, or called, without an error, and change nothing.

- **Services that need Roblox's own servers**: AssetService, BadgeService, ContentProvider, GamepadService, GroupService, GuiService, HapticService, LocalizationService, MarketplaceService, MemoryStoreService, MessagingService, MouseService, PhysicsService, PolicyService, ProximityPromptService, SocialService, Stats, TeleportService, TextService, VRService
- **BasePart**: BackSurface, BottomSurface, CollisionGroup, CustomPhysicalProperties, EnableFluidForces, FrontSurface, LeftSurface, LocalTransparencyModifier, Locked, Massless, PivotOffset, RightSurface, RootPriority, TopSurface
- **Decal**: ZIndex
- **GuiObject**: Active, AutomaticSize, NextSelectionDown, NextSelectionLeft, NextSelectionRight, NextSelectionUp, Selectable, SelectionImageObject, SelectionOrder
- **Highlight**: DepthMode
- **Humanoid**: AutomaticScalingEnabled, BreakJointsOnDeath, DisplayDistanceType, EvaluateStateMachine, HipHeight, PlatformStand, RequiresNeck, UseJumpPower, WalkToPart, WalkToPoint
- **ImageButton**: HoverImage, ImageRectOffset, ImageRectSize, PressedImage, SliceCenter, SliceScale, TileSize
- **ImageLabel**: ImageRectOffset, ImageRectSize, SliceCenter, SliceScale, TileSize
- **Lighting**: ColorShift_Bottom, ColorShift_Top, EnvironmentDiffuseScale, EnvironmentSpecularScale
- **MeshPart**: TextureID
- **Model**: LevelOfDetail, ModelStreamingMode, WorldPivot
- **Motor6D**: Active, CurrentAngle, DesiredAngle, MaxVelocity
- **ParticleEmitter**: WindAffectsDrag
- **Player**: AccountAge, AutoJumpEnabled, CameraMaxZoomDistance, CameraMinZoomDistance, CameraMode, DevEnableMouseLock, HealthDisplayDistance, LocaleId, MembershipType, NameDisplayDistance, RespawnLocation
- **ProximityPrompt**: AutoLocalize, ClickablePrompt, Style
- **ScreenGui**: ClipToDeviceSafeArea, IgnoreGuiInset, ScreenInsets, ZIndexBehavior
- **ScrollingFrame**: AutomaticCanvasSize, ElasticBehavior, ScrollBarImageColor3, ScrollingDirection, ScrollingEnabled
- **Sound**: PlayOnRemove, RollOffMode
- **SoundService**: DistanceFactor, DopplerScale, RespectFilteringEnabled
- **SpawnLocation**: Duration
- **SpotLight**: Face
- **StarterGui**: ResetPlayerGuiOnSpawn, ScreenOrientation, ShowDevelopmentGui
- **StarterPlayer**: AutoJumpEnabled, CharacterJumpHeight, CharacterUseJumpPower, DevComputerCameraMode, DevComputerMovementMode, EnableMouseLockOption, LoadCharacterAppearance
- **Terrain**: WaterReflectance, WaterWaveSize, WaterWaveSpeed
- **TextBox**: CursorPosition, FontFace, LineHeight, MaxVisibleGraphemes, MultiLine, RichText, SelectionStart, TextEditable, TextTruncate
- **TextButton**: FontFace, LineHeight, MaxVisibleGraphemes, Modal, RichText, Selected, TextTruncate
- **TextLabel**: FontFace, LineHeight, MaxVisibleGraphemes, RichText, TextTruncate
- **Tool**: GripForward, GripRight, GripUp
- **Workspace**: AllowThirdPartySales, DistributedGameTime, FilteringEnabled, StreamingEnabled
