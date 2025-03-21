#[cfg(feature = "trace")]
use crate::device::trace;
use crate::{
    binding_model::{BindGroup, BindGroupLayout, PipelineLayout},
    command::RenderBundle,
    device::{
        queue::{EncoderInFlight, SubmittedWorkDoneClosure, TempResource},
        DeviceError, DeviceLostClosure,
    },
    hal_api::HalApi,
    id::{
        self, BindGroupId, BindGroupLayoutId, BufferId, ComputePipelineId, PipelineLayoutId,
        QuerySetId, RenderBundleId, RenderPipelineId, SamplerId, StagingBufferId, TextureId,
        TextureViewId,
    },
    pipeline::{ComputePipeline, RenderPipeline},
    resource::{
        self, Buffer, QuerySet, Resource, ResourceType, Sampler, StagingBuffer, Texture,
        TextureView,
    },
    track::{ResourceTracker, Tracker},
    FastHashMap, SubmissionIndex,
};
use smallvec::SmallVec;

use parking_lot::Mutex;
use thiserror::Error;
use wgt::WasmNotSendSync;

use std::{any::Any, sync::Arc};

pub(crate) trait ResourceMap: Any + WasmNotSendSync {
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn clear_map(&mut self);
    fn extend_map(&mut self, maps: &mut ResourceMaps);
}

impl<Id, R> ResourceMap for FastHashMap<Id, Arc<R>>
where
    Id: id::TypedId,
    R: Resource<Id>,
{
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn clear_map(&mut self) {
        self.clear()
    }
    fn extend_map(&mut self, r: &mut ResourceMaps) {
        if let Some(other) = r.maps.get_mut(R::TYPE) {
            if let Some(other) = other.as_any_mut().downcast_mut::<Self>() {
                self.extend(other.drain());
            }
        }
    }
}

/// A struct that keeps lists of resources that are no longer needed by the user.
#[derive(Default)]
pub(crate) struct ResourceMaps {
    pub(crate) maps: FastHashMap<ResourceType, Box<dyn ResourceMap>>,
}

impl ResourceMaps {
    fn add_type<Id, R>(&mut self) -> &mut Self
    where
        Id: id::TypedId,
        R: Resource<Id>,
    {
        let map = FastHashMap::<Id, Arc<R>>::default();
        self.maps.insert(R::TYPE, Box::new(map));
        self
    }
    fn map_mut<Id, R>(&mut self) -> &mut FastHashMap<Id, Arc<R>>
    where
        Id: id::TypedId,
        R: Resource<Id>,
    {
        let map = self
            .maps
            .entry(R::TYPE)
            .or_insert_with(|| Box::<FastHashMap<Id, Arc<R>>>::default());
        let any_map = map.as_mut().as_any_mut();
        let map = any_map.downcast_mut::<FastHashMap<Id, Arc<R>>>().unwrap();
        map
    }
    pub(crate) fn new<A: HalApi>() -> Self {
        let mut maps = Self::default();
        maps.add_type::<BufferId, Buffer<A>>();
        maps.add_type::<StagingBufferId, StagingBuffer<A>>();
        maps.add_type::<TextureId, Texture<A>>();
        maps.add_type::<TextureViewId, TextureView<A>>();
        maps.add_type::<SamplerId, Sampler<A>>();
        maps.add_type::<BindGroupId, BindGroup<A>>();
        maps.add_type::<BindGroupLayoutId, BindGroupLayout<A>>();
        maps.add_type::<RenderPipelineId, RenderPipeline<A>>();
        maps.add_type::<ComputePipelineId, ComputePipeline<A>>();
        maps.add_type::<PipelineLayoutId, PipelineLayout<A>>();
        maps.add_type::<RenderBundleId, RenderBundle<A>>();
        maps.add_type::<QuerySetId, QuerySet<A>>();
        maps
    }
    pub(crate) fn clear(&mut self) {
        self.maps.iter_mut().for_each(|(_t, map)| map.clear_map());
    }
    pub(crate) fn extend(&mut self, mut other: Self) {
        self.maps.iter_mut().for_each(|(_t, map)| {
            map.extend_map(&mut other);
        });
    }
    pub(crate) fn insert<Id, R>(&mut self, id: Id, r: Arc<R>) -> &mut Self
    where
        Id: id::TypedId,
        R: Resource<Id>,
    {
        self.map_mut().insert(id, r);
        self
    }
}

/// Resources used by a queue submission, and work to be done once it completes.
struct ActiveSubmission<A: HalApi> {
    /// The index of the submission we track.
    ///
    /// When `Device::fence`'s value is greater than or equal to this, our queue
    /// submission has completed.
    index: SubmissionIndex,

    /// Resources to be freed once this queue submission has completed.
    ///
    /// When the device is polled, for completed submissions,
    /// `triage_submissions` merges these into
    /// `LifetimeTracker::free_resources`. From there,
    /// `LifetimeTracker::cleanup` passes them to the hal to be freed.
    ///
    /// This includes things like temporary resources and resources that are
    /// used by submitted commands but have been dropped by the user (meaning that
    /// this submission is their last reference.)
    last_resources: ResourceMaps,

    /// Buffers to be mapped once this submission has completed.
    mapped: Vec<Arc<Buffer<A>>>,

    encoders: Vec<EncoderInFlight<A>>,

    /// List of queue "on_submitted_work_done" closures to be called once this
    /// submission has completed.
    work_done_closures: SmallVec<[SubmittedWorkDoneClosure; 1]>,
}

#[derive(Clone, Debug, Error)]
#[non_exhaustive]
pub enum WaitIdleError {
    #[error(transparent)]
    Device(#[from] DeviceError),
    #[error("Tried to wait using a submission index from the wrong device. Submission index is from device {0:?}. Called poll on device {1:?}.")]
    WrongSubmissionIndex(id::QueueId, id::DeviceId),
    #[error("GPU got stuck :(")]
    StuckGpu,
}

/// Resource tracking for a device.
///
/// ## Host mapping buffers
///
/// A buffer cannot be mapped until all active queue submissions that use it
/// have completed. To that end:
///
/// -   Each buffer's `ResourceInfo::submission_index` records the index of the
///     most recent queue submission that uses that buffer.
///
/// -   Calling `Global::buffer_map_async` adds the buffer to
///     `self.mapped`, and changes `Buffer::map_state` to prevent it
///     from being used in any new submissions.
///
/// -   When the device is polled, the following `LifetimeTracker` methods decide
///     what should happen next:
///
///     1)  `triage_mapped` drains `self.mapped`, checking the submission index
///         of each buffer against the queue submissions that have finished
///         execution. Buffers used by submissions still in flight go in
///         `self.active[index].mapped`, and the rest go into
///         `self.ready_to_map`.
///
///     2)  `triage_submissions` moves entries in `self.active[i]` for completed
///         submissions to `self.ready_to_map`.  At this point, both
///         `self.active` and `self.ready_to_map` are up to date with the given
///         submission index.
///
///     3)  `handle_mapping` drains `self.ready_to_map` and actually maps the
///         buffers, collecting a list of notification closures to call. But any
///         buffers that were dropped by the user get moved to
///         `self.free_resources`.
///
///     4)  `cleanup` frees everything in `free_resources`.
///
/// Only calling `Global::buffer_map_async` clones a new `Arc` for the
/// buffer. This new `Arc` is only dropped by `handle_mapping`.
pub(crate) struct LifetimeTracker<A: HalApi> {
    /// Resources that the user has requested be mapped, but which are used by
    /// queue submissions still in flight.
    mapped: Vec<Arc<Buffer<A>>>,

    /// Buffers can be used in a submission that is yet to be made, by the
    /// means of `write_buffer()`, so we have a special place for them.
    pub future_suspected_buffers: Vec<Arc<Buffer<A>>>,

    /// Textures can be used in the upcoming submission by `write_texture`.
    pub future_suspected_textures: Vec<Arc<Texture<A>>>,

    /// Resources whose user handle has died (i.e. drop/destroy has been called)
    /// and will likely be ready for destruction soon.
    pub suspected_resources: ResourceMaps,

    /// Resources used by queue submissions still in flight. One entry per
    /// submission, with older submissions appearing before younger.
    ///
    /// Entries are added by `track_submission` and drained by
    /// `LifetimeTracker::triage_submissions`. Lots of methods contribute data
    /// to particular entries.
    active: Vec<ActiveSubmission<A>>,

    /// Raw backend resources that are neither referenced nor used.
    ///
    /// These are freed by `LifeTracker::cleanup`, which is called from periodic
    /// maintenance functions like `Global::device_poll`, and when a device is
    /// destroyed.
    free_resources: ResourceMaps,

    /// Buffers the user has asked us to map, and which are not used by any
    /// queue submission still in flight.
    ready_to_map: Vec<Arc<Buffer<A>>>,

    /// Queue "on_submitted_work_done" closures that were initiated for while there is no
    /// currently pending submissions. These cannot be immeidately invoked as they
    /// must happen _after_ all mapped buffer callbacks are mapped, so we defer them
    /// here until the next time the device is maintained.
    work_done_closures: SmallVec<[SubmittedWorkDoneClosure; 1]>,

    /// Closure to be called on "lose the device". This is invoked directly by
    /// device.lose or by the UserCallbacks returned from maintain when the device
    /// has been destroyed and its queues are empty.
    pub device_lost_closure: Option<DeviceLostClosure>,
}

impl<A: HalApi> LifetimeTracker<A> {
    pub fn new() -> Self {
        Self {
            mapped: Vec::new(),
            future_suspected_buffers: Vec::new(),
            future_suspected_textures: Vec::new(),
            suspected_resources: ResourceMaps::new::<A>(),
            active: Vec::new(),
            free_resources: ResourceMaps::new::<A>(),
            ready_to_map: Vec::new(),
            work_done_closures: SmallVec::new(),
            device_lost_closure: None,
        }
    }

    /// Return true if there are no queue submissions still in flight.
    pub fn queue_empty(&self) -> bool {
        self.active.is_empty()
    }

    /// Start tracking resources associated with a new queue submission.
    pub fn track_submission(
        &mut self,
        index: SubmissionIndex,
        temp_resources: impl Iterator<Item = TempResource<A>>,
        encoders: Vec<EncoderInFlight<A>>,
    ) {
        let mut last_resources = ResourceMaps::new::<A>();
        for res in temp_resources {
            match res {
                TempResource::Buffer(raw) => {
                    last_resources.insert(raw.as_info().id(), raw);
                }
                TempResource::StagingBuffer(raw) => {
                    last_resources.insert(raw.as_info().id(), raw);
                }
                TempResource::Texture(raw) => {
                    last_resources.insert(raw.as_info().id(), raw);
                }
            }
        }

        self.active.push(ActiveSubmission {
            index,
            last_resources,
            mapped: Vec::new(),
            encoders,
            work_done_closures: SmallVec::new(),
        });
    }

    pub fn post_submit(&mut self) {
        for v in self.future_suspected_buffers.drain(..).take(1) {
            self.suspected_resources.insert(v.as_info().id(), v);
        }
        for v in self.future_suspected_textures.drain(..).take(1) {
            self.suspected_resources.insert(v.as_info().id(), v);
        }
    }

    pub(crate) fn map(&mut self, value: &Arc<Buffer<A>>) {
        self.mapped.push(value.clone());
    }

    /// Sort out the consequences of completed submissions.
    ///
    /// Assume that all submissions up through `last_done` have completed.
    ///
    /// -   Buffers used by those submissions are now ready to map, if
    ///     requested. Add any buffers in the submission's [`mapped`] list to
    ///     [`self.ready_to_map`], where [`LifetimeTracker::handle_mapping`] will find
    ///     them.
    ///
    /// -   Resources whose final use was in those submissions are now ready to
    ///     free. Add any resources in the submission's [`last_resources`] table
    ///     to [`self.free_resources`], where [`LifetimeTracker::cleanup`] will find
    ///     them.
    ///
    /// Return a list of [`SubmittedWorkDoneClosure`]s to run.
    ///
    /// [`mapped`]: ActiveSubmission::mapped
    /// [`self.ready_to_map`]: LifetimeTracker::ready_to_map
    /// [`last_resources`]: ActiveSubmission::last_resources
    /// [`self.free_resources`]: LifetimeTracker::free_resources
    /// [`SubmittedWorkDoneClosure`]: crate::device::queue::SubmittedWorkDoneClosure
    #[must_use]
    pub fn triage_submissions(
        &mut self,
        last_done: SubmissionIndex,
        command_allocator: &mut super::CommandAllocator<A>,
    ) -> SmallVec<[SubmittedWorkDoneClosure; 1]> {
        profiling::scope!("triage_submissions");

        //TODO: enable when `is_sorted_by_key` is stable
        //debug_assert!(self.active.is_sorted_by_key(|a| a.index));
        let done_count = self
            .active
            .iter()
            .position(|a| a.index > last_done)
            .unwrap_or(self.active.len());

        let mut work_done_closures: SmallVec<_> = self.work_done_closures.drain(..).collect();
        for a in self.active.drain(..done_count) {
            log::debug!("Active submission {} is done", a.index);
            self.free_resources.extend(a.last_resources);
            self.ready_to_map.extend(a.mapped);
            for encoder in a.encoders {
                let raw = unsafe { encoder.land() };
                command_allocator.release_encoder(raw);
            }
            work_done_closures.extend(a.work_done_closures);
        }
        work_done_closures
    }

    pub fn cleanup(&mut self) {
        profiling::scope!("LifetimeTracker::cleanup");
        self.free_resources.clear();
    }

    pub fn schedule_resource_destruction(
        &mut self,
        temp_resource: TempResource<A>,
        last_submit_index: SubmissionIndex,
    ) {
        let resources = self
            .active
            .iter_mut()
            .find(|a| a.index == last_submit_index)
            .map_or(&mut self.free_resources, |a| &mut a.last_resources);
        match temp_resource {
            TempResource::Buffer(raw) => {
                resources.insert(raw.as_info().id(), raw);
            }
            TempResource::StagingBuffer(raw) => {
                resources.insert(raw.as_info().id(), raw);
            }
            TempResource::Texture(raw) => {
                resources.insert(raw.as_info().id(), raw);
            }
        }
    }

    pub fn add_work_done_closure(&mut self, closure: SubmittedWorkDoneClosure) {
        match self.active.last_mut() {
            Some(active) => {
                active.work_done_closures.push(closure);
            }
            // We must defer the closure until all previously occuring map_async closures
            // have fired. This is required by the spec.
            None => {
                self.work_done_closures.push(closure);
            }
        }
    }
}

impl<A: HalApi> LifetimeTracker<A> {
    fn triage_resources<Id, R, T>(
        resources_map: &mut FastHashMap<Id, Arc<R>>,
        active: &mut [ActiveSubmission<A>],
        free_resources: &mut ResourceMaps,
        trackers: &mut impl ResourceTracker<Id, R>,
        mut on_remove: T,
    ) -> Vec<Arc<R>>
    where
        Id: id::TypedId,
        R: Resource<Id>,
        T: FnMut(&Id, &Arc<R>),
    {
        let mut removed_resources = Vec::new();
        resources_map.retain(|&id, resource| {
            let submit_index = resource.as_info().submission_index();
            let non_referenced_resources = active
                .iter_mut()
                .find(|a| a.index == submit_index)
                .map_or(&mut *free_resources, |a| &mut a.last_resources);

            let is_removed = trackers.remove_abandoned(id);
            if is_removed {
                on_remove(&id, resource);
                removed_resources.push(resource.clone());
                non_referenced_resources.insert(id, resource.clone());
            }
            !is_removed
        });
        removed_resources
    }

    fn triage_suspected_render_bundles(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resources = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.bundles,
            |_bundle_id, _bundle| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyRenderBundle(*_bundle_id));
                }
            },
        );
        removed_resources.drain(..).for_each(|bundle| {
            for v in bundle.used.buffers.write().drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bundle.used.textures.write().drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bundle.used.bind_groups.write().drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bundle.used.render_pipelines.write().drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bundle.used.query_sets.write().drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
        });
        self
    }

    fn triage_suspected_bind_groups(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resource = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.bind_groups,
            |_bind_group_id, _bind_group| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyBindGroup(*_bind_group_id));
                }
            },
        );
        removed_resource.drain(..).for_each(|bind_group| {
            for v in bind_group.used.buffers.drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bind_group.used.textures.drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bind_group.used.views.drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            for v in bind_group.used.samplers.drain_resources() {
                self.suspected_resources.insert(v.as_info().id(), v);
            }
            //Releasing safely unused resources to decrement refcount
            bind_group.used_buffer_ranges.write().clear();
            bind_group.used_texture_ranges.write().clear();
            bind_group.dynamic_binding_info.write().clear();

            self.suspected_resources
                .insert(bind_group.layout.as_info().id(), bind_group.layout.clone());
        });
        self
    }

    fn triage_suspected_texture_views(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resources = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.views,
            |_texture_view_id, _texture_view| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyTextureView(*_texture_view_id));
                }
            },
        );
        removed_resources.drain(..).for_each(|texture_view| {
            let mut lock = texture_view.parent.write();
            if let Some(parent_texture) = lock.take() {
                self.suspected_resources
                    .insert(parent_texture.as_info().id(), parent_texture);
            }
        });
        self
    }

    fn triage_suspected_textures(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.textures,
            |_texture_id, _texture| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyTexture(*_texture_id));
                }
            },
        );
        self
    }

    fn triage_suspected_samplers(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.samplers,
            |_sampler_id, _sampler| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroySampler(*_sampler_id));
                }
            },
        );
        self
    }

    fn triage_suspected_buffers(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resources = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.buffers,
            |_buffer_id, _buffer| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyBuffer(*_buffer_id));
                }
            },
        );
        removed_resources.drain(..).for_each(|buffer| {
            if let resource::BufferMapState::Init {
                ref stage_buffer, ..
            } = *buffer.map_state.lock()
            {
                self.free_resources
                    .insert(stage_buffer.as_info().id(), stage_buffer.clone());
            }
        });
        self
    }

    fn triage_suspected_compute_pipelines(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resources = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.compute_pipelines,
            |_compute_pipeline_id, _compute_pipeline| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyComputePipeline(*_compute_pipeline_id));
                }
            },
        );
        removed_resources.drain(..).for_each(|compute_pipeline| {
            self.suspected_resources.insert(
                compute_pipeline.layout.as_info().id(),
                compute_pipeline.layout.clone(),
            );
        });
        self
    }

    fn triage_suspected_render_pipelines(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        let mut removed_resources = Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.render_pipelines,
            |_render_pipeline_id, _render_pipeline| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyRenderPipeline(*_render_pipeline_id));
                }
            },
        );
        removed_resources.drain(..).for_each(|render_pipeline| {
            self.suspected_resources.insert(
                render_pipeline.layout.as_info().id(),
                render_pipeline.layout.clone(),
            );
        });
        self
    }

    fn triage_suspected_pipeline_layouts(
        &mut self,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        let mut removed_resources = Vec::new();
        self.suspected_resources
            .map_mut::<PipelineLayoutId, PipelineLayout<A>>()
            .retain(|_pipeline_layout_id, pipeline_layout| {
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyPipelineLayout(*_pipeline_layout_id));
                }
                removed_resources.push(pipeline_layout.clone());
                false
            });
        removed_resources.drain(..).for_each(|pipeline_layout| {
            for bgl in &pipeline_layout.bind_group_layouts {
                self.suspected_resources
                    .insert(bgl.as_info().id(), bgl.clone());
            }
        });
        self
    }

    fn triage_suspected_bind_group_layouts(
        &mut self,
        #[cfg(feature = "trace")] trace: &mut Option<&mut trace::Trace>,
    ) -> &mut Self {
        self.suspected_resources
            .map_mut::<BindGroupLayoutId, BindGroupLayout<A>>()
            .retain(|bind_group_layout_id, bind_group_layout| {
                //Note: this has to happen after all the suspected pipelines are destroyed
                //Note: nothing else can bump the refcount since the guard is locked exclusively
                //Note: same BGL can appear multiple times in the list, but only the last
                // encounter could drop the refcount to 0.
                #[cfg(feature = "trace")]
                if let Some(ref mut t) = *trace {
                    t.add(trace::Action::DestroyBindGroupLayout(*bind_group_layout_id));
                }
                self.free_resources
                    .insert(*bind_group_layout_id, bind_group_layout.clone());
                false
            });
        self
    }

    fn triage_suspected_query_sets(&mut self, trackers: &Mutex<Tracker<A>>) -> &mut Self {
        let mut trackers = trackers.lock();
        let resource_map = self.suspected_resources.map_mut();
        Self::triage_resources(
            resource_map,
            self.active.as_mut_slice(),
            &mut self.free_resources,
            &mut trackers.query_sets,
            |_query_set_id, _query_set| {},
        );
        self
    }

    fn triage_suspected_staging_buffers(&mut self) -> &mut Self {
        self.suspected_resources
            .map_mut::<StagingBufferId, StagingBuffer<A>>()
            .retain(|staging_buffer_id, staging_buffer| {
                self.free_resources
                    .insert(*staging_buffer_id, staging_buffer.clone());
                false
            });
        self
    }

    /// Identify resources to free, according to `trackers` and `self.suspected_resources`.
    ///
    /// Given `trackers`, the [`Tracker`] belonging to same [`Device`] as
    /// `self`, and `hub`, the [`Hub`] to which that `Device` belongs:
    ///
    /// Remove from `trackers` each resource mentioned in
    /// [`self.suspected_resources`]. If `trackers` held the final reference to
    /// that resource, add it to the appropriate free list, to be destroyed by
    /// the hal:
    ///
    /// -   Add resources used by queue submissions still in flight to the
    ///     [`last_resources`] table of the last such submission's entry in
    ///     [`self.active`]. When that submission has finished execution. the
    ///     [`triage_submissions`] method will move them to
    ///     [`self.free_resources`].
    ///
    /// -   Add resources that can be freed right now to [`self.free_resources`]
    ///     directly. [`LifetimeTracker::cleanup`] will take care of them as
    ///     part of this poll.
    ///
    /// ## Entrained resources
    ///
    /// This function finds resources that are used only by other resources
    /// ready to be freed, and adds those to the free lists as well. For
    /// example, if there's some texture `T` used only by some texture view
    /// `TV`, then if `TV` can be freed, `T` gets added to the free lists too.
    ///
    /// Since `wgpu-core` resource ownership patterns are acyclic, we can visit
    /// each type that can be owned after all types that could possibly own
    /// it. This way, we can detect all free-able objects in a single pass,
    /// simply by starting with types that are roots of the ownership DAG (like
    /// render bundles) and working our way towards leaf types (like buffers).
    ///
    /// [`Device`]: super::Device
    /// [`self.suspected_resources`]: LifetimeTracker::suspected_resources
    /// [`last_resources`]: ActiveSubmission::last_resources
    /// [`self.active`]: LifetimeTracker::active
    /// [`triage_submissions`]: LifetimeTracker::triage_submissions
    /// [`self.free_resources`]: LifetimeTracker::free_resources
    pub(crate) fn triage_suspected(
        &mut self,
        trackers: &Mutex<Tracker<A>>,
        #[cfg(feature = "trace")] mut trace: Option<&mut trace::Trace>,
    ) {
        profiling::scope!("triage_suspected");

        //NOTE: the order is important to release resources that depends between each other!
        self.triage_suspected_render_bundles(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_compute_pipelines(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_render_pipelines(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_bind_groups(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_pipeline_layouts(
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_bind_group_layouts(
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_query_sets(trackers);
        self.triage_suspected_samplers(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_staging_buffers();
        self.triage_suspected_texture_views(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_textures(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
        self.triage_suspected_buffers(
            trackers,
            #[cfg(feature = "trace")]
            &mut trace,
        );
    }

    /// Determine which buffers are ready to map, and which must wait for the
    /// GPU.
    ///
    /// See the documentation for [`LifetimeTracker`] for details.
    pub(crate) fn triage_mapped(&mut self) {
        if self.mapped.is_empty() {
            return;
        }

        for buffer in self.mapped.drain(..) {
            let submit_index = buffer.info.submission_index();
            log::trace!(
                "Mapping of {:?} at submission {:?} gets assigned to active {:?}",
                buffer.info.id(),
                submit_index,
                self.active.iter().position(|a| a.index == submit_index)
            );

            self.active
                .iter_mut()
                .find(|a| a.index == submit_index)
                .map_or(&mut self.ready_to_map, |a| &mut a.mapped)
                .push(buffer);
        }
    }

    /// Map the buffers in `self.ready_to_map`.
    ///
    /// Return a list of mapping notifications to send.
    ///
    /// See the documentation for [`LifetimeTracker`] for details.
    #[must_use]
    pub(crate) fn handle_mapping(
        &mut self,
        raw: &A::Device,
        trackers: &Mutex<Tracker<A>>,
    ) -> Vec<super::BufferMapPendingClosure> {
        if self.ready_to_map.is_empty() {
            return Vec::new();
        }
        let mut pending_callbacks: Vec<super::BufferMapPendingClosure> =
            Vec::with_capacity(self.ready_to_map.len());

        for buffer in self.ready_to_map.drain(..) {
            let buffer_id = buffer.info.id();
            let is_removed = {
                let mut trackers = trackers.lock();
                trackers.buffers.remove_abandoned(buffer_id)
            };
            if is_removed {
                *buffer.map_state.lock() = resource::BufferMapState::Idle;
                log::trace!("Buffer ready to map {:?} is not tracked anymore", buffer_id);
                self.free_resources.insert(buffer_id, buffer.clone());
            } else {
                let mapping = match std::mem::replace(
                    &mut *buffer.map_state.lock(),
                    resource::BufferMapState::Idle,
                ) {
                    resource::BufferMapState::Waiting(pending_mapping) => pending_mapping,
                    // Mapping cancelled
                    resource::BufferMapState::Idle => continue,
                    // Mapping queued at least twice by map -> unmap -> map
                    // and was already successfully mapped below
                    active @ resource::BufferMapState::Active { .. } => {
                        *buffer.map_state.lock() = active;
                        continue;
                    }
                    _ => panic!("No pending mapping."),
                };
                let status = if mapping.range.start != mapping.range.end {
                    log::debug!("Buffer {:?} map state -> Active", buffer_id);
                    let host = mapping.op.host;
                    let size = mapping.range.end - mapping.range.start;
                    match super::map_buffer(raw, &buffer, mapping.range.start, size, host) {
                        Ok(ptr) => {
                            *buffer.map_state.lock() = resource::BufferMapState::Active {
                                ptr,
                                range: mapping.range.start..mapping.range.start + size,
                                host,
                            };
                            Ok(())
                        }
                        Err(e) => {
                            log::error!("Mapping failed: {e}");
                            Err(e)
                        }
                    }
                } else {
                    *buffer.map_state.lock() = resource::BufferMapState::Active {
                        ptr: std::ptr::NonNull::dangling(),
                        range: mapping.range,
                        host: mapping.op.host,
                    };
                    Ok(())
                };
                pending_callbacks.push((mapping.op, status));
            }
        }
        pending_callbacks
    }
}
