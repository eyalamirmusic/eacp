import { useMemo } from 'react';
import { backend } from './generated/backend';
import { useHubState } from './generated/hooks';
import type {
    HubInstallState,
    HubOperation,
    HubOperationKind,
    HubOperationState,
    HubProduct,
    HubProductKind,
    RemoteAppStatus,
} from './generated/schema';

const installStateLabels: Record<HubInstallState, string> = {
    NotInstalled: 'Not installed',
    Installed: 'Installed',
    UpdateAvailable: 'Update available',
    Running: 'Running',
};

const productKindLabels: Record<HubProductKind, string> = {
    App: 'App',
    Runtime: 'Runtime',
    Model: 'Model',
    Blob: 'Blob',
};

const operationStateLabels: Record<HubOperationState, string> = {
    Idle: 'Idle',
    Working: 'Working',
    Succeeded: 'Done',
    Failed: 'Failed',
};

const operationKindLabels: Record<HubOperationKind, string> = {
    None: 'Ready',
    Checking: 'Checking',
    DownloadingManifest: 'Manifest',
    DownloadingArtifact: 'Downloading',
    VerifyingArtifact: 'Verifying',
    Installing: 'Installing',
    Launching: 'Launching',
    Updating: 'Updating',
    Resetting: 'Resetting',
};

export default function App()
{
    const state = useHubState();
    const apps = useMemo(
        () => state.products.filter((product) => product.kind === 'App'),
        [state.products],
    );
    const shared = useMemo(
        () => state.products.filter((product) => product.kind !== 'App'),
        [state.products],
    );

    return (
        <main>
            <header className="topbar">
                <div>
                    <h1>Tamber AppHub</h1>
                    <p>Hub {state.hubVersion}</p>
                </div>
                <div className="topbar-actions">
                    <button type="button" onClick={() => void backend.refresh()}>
                        Refresh
                    </button>
                    <button type="button" onClick={() => void backend.checkUpdates()}>
                        Check updates
                    </button>
                </div>
            </header>

            <OperationStrip operation={state.operation} />

            <section className="grid remote-grid single">
                <RemoteCard
                    title="AppHub"
                    status={state.hubApp}
                    installLabel={state.hubApp.updateAvailable ? 'Update Hub' : 'Up to date'}
                    launchLabel="Open Installed Hub"
                    installDisabled={!state.hubApp.updateAvailable}
                    onInstall={() => void backend.updateHub({ manifestUrl: '' })}
                    onLaunch={() => void backend.launchHub()}
                />
            </section>

            <section className="section">
                <div className="section-head">
                    <div>
                        <h2>Available Apps</h2>
                        <span className="muted">
                            Signed builds published by Tamber CI.
                        </span>
                    </div>
                    <div className="button-row">
                        <button type="button" onClick={() => void backend.updateAll()}>
                            Update all
                        </button>
                    </div>
                </div>
                <div className="product-list">
                    {apps.map((product) => <ProductRow key={product.id} product={product} />)}
                </div>
            </section>

            <section className="section">
                <div className="section-head">
                    <div>
                        <h2>Installed Shared Resources</h2>
                        <span className="muted">
                            Dependencies installed once and reused across apps.
                        </span>
                    </div>
                </div>
                <div className="resource-grid">
                    {shared.map((product) => <ResourceCard key={product.id} product={product} />)}
                </div>
            </section>
        </main>
    );
}

function OperationStrip({ operation }: { operation: HubOperation })
{
    const percent = operation.totalBytes > 0
        ? Math.min(100, Math.round(operation.bytesReceived * 100 / operation.totalBytes))
        : operation.state === 'Succeeded'
            ? 100
            : 0;
    const working = operation.state === 'Working';

    return (
        <section className={`operation ${operation.state.toLowerCase()}`}>
            <div>
                <span className="eyebrow">
                    {operationKindLabels[operation.kind]} · {operationStateLabels[operation.state]}
                </span>
                <strong>{operation.title || 'Ready'}</strong>
                <p>{operation.detail || 'No operation running.'}</p>
            </div>
            <div className="progress-wrap" aria-hidden={!working}>
                <div className="progress-meta">
                    <span>{formatBytes(operation.bytesReceived)}</span>
                    <span>{operation.totalBytes > 0 ? `${percent}%` : ''}</span>
                </div>
                <div className="progress">
                    <div style={{ width: `${working ? Math.max(percent, 8) : percent}%` }} />
                </div>
            </div>
        </section>
    );
}

interface RemoteCardProps
{
    title: string;
    status: RemoteAppStatus;
    installLabel: string;
    launchLabel: string;
    installDisabled?: boolean;
    onInstall: () => void;
    onLaunch: () => void;
}

function RemoteCard({
    title,
    status,
    installLabel,
    launchLabel,
    installDisabled = false,
    onInstall,
    onLaunch,
}: RemoteCardProps)
{
    return (
        <article className="remote-card">
            <div>
                <span className="eyebrow">{title}</span>
                <h2>{status.name || title}</h2>
                <p>{status.message || 'Waiting for update check.'}</p>
            </div>
            <dl>
                <Metric label="Installed" value={status.installedVersion || '-'} />
                <Metric label="Latest" value={status.latestVersion || '-'} />
            </dl>
            <div className="button-row">
                <button
                    type="button"
                    className="primary"
                    onClick={onInstall}
                    disabled={installDisabled}
                >
                    {installLabel}
                </button>
                <button type="button" onClick={onLaunch} disabled={!status.installed}>
                    {launchLabel}
                </button>
            </div>
        </article>
    );
}

function ProductRow({ product }: { product: HubProduct })
{
    const installed = product.state !== 'NotInstalled';
    const running = product.state === 'Running';

    return (
        <article className="product-row">
            <div className="product-main">
                <span className={`status-dot ${product.state.toLowerCase()}`} />
                <div>
                    <h3>{product.name}</h3>
                    <p>{product.id}</p>
                </div>
            </div>
            <div className="product-meta">
                <span>{installStateLabels[product.state]}</span>
                <span>{product.installedVersion || '-'} / {product.latestVersion || '-'}</span>
            </div>
            <div className="button-row">
                <button
                    type="button"
                    className="primary"
                    onClick={() => void backend.installProduct({ productId: product.id })}
                >
                    {product.state === 'UpdateAvailable'
                        ? 'Update'
                        : installed
                            ? 'Reinstall'
                            : 'Install'}
                </button>
                <button
                    type="button"
                    onClick={() => void backend.openProduct({ productId: product.id })}
                    disabled={!installed || running}
                >
                    Open
                </button>
                <button
                    type="button"
                    onClick={() => void backend.closeProduct({ productId: product.id })}
                    disabled={!running}
                >
                    Close
                </button>
            </div>
        </article>
    );
}

function ResourceCard({ product }: { product: HubProduct })
{
    return (
        <article className="resource-card">
            <span className="eyebrow">{productKindLabels[product.kind]}</span>
            <h3>{product.name}</h3>
            <p>{product.id}</p>
            <div className="resource-footer">
                <span>{installStateLabels[product.state]}</span>
                <span>{product.installedVersion || product.latestVersion || '-'}</span>
            </div>
        </article>
    );
}

function Metric({ label, value }: { label: string; value: string })
{
    return (
        <div>
            <dt>{label}</dt>
            <dd>{value}</dd>
        </div>
    );
}

function formatBytes(bytes: number): string
{
    if (bytes <= 0) return '';
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}
