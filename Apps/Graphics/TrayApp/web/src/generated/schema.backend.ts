import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        searchDownloads: (req: T.SearchDownloadsRequest): Promise<T.SearchDownloadsResponse> =>
            invoke('searchDownloads', req) as Promise<T.SearchDownloadsResponse>,
        armDrag: (req: T.ArmDragRequest): Promise<void> =>
            invoke('armDrag', req) as Promise<void>,
        copyFiles: (req: T.CopyFilesRequest): Promise<T.CopyFilesResponse> =>
            invoke('copyFiles', req) as Promise<T.CopyFilesResponse>,
        showCopyToast: (req: T.ShowCopyToastRequest): Promise<void> =>
            invoke('showCopyToast', req) as Promise<void>,
        playAudio: (req: T.PlayAudioRequest): Promise<void> =>
            invoke('playAudio', req) as Promise<void>,
        stopAudio: (): Promise<void> =>
            invoke('stopAudio', {}) as Promise<void>,
        getPlayback: (): Promise<T.PlaybackState> =>
            invoke('getPlayback', {}) as Promise<T.PlaybackState>,
        submitPrompt: (req: T.SubmitPromptRequest): Promise<void> =>
            invoke('submitPrompt', req) as Promise<void>,
        dismiss: (): Promise<void> =>
            invoke('dismiss', {}) as Promise<void>,
    };
}
