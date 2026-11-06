#!/usr/bin/env bash
# Creates, or brings up to date, everything the llm-docs workflow needs in GCP:
# a public-read bucket, a service account that may write only to it, and a
# Workload Identity Federation pool whose provider accepts GitHub Actions
# tokens from the listed repositories alone. Safe to run again.
#
# Usage: Scripts/setup-docs-bucket.sh   (as an account that owns the project;
#        CLOUDSDK_CORE_ACCOUNT picks one other than gcloud's active account)
set -euo pipefail

project=${GCP_PROJECT:-pond-audio}
bucket=gs://${EACP_DOCS_BUCKET:-eacp-docs}
location=${EACP_DOCS_LOCATION:-us}
service_account_name=eacp-docs-publisher
pool=github
provider=github-actions
repositories=(eyalamirmusic/eacp jamierpond/eacp)

service_account=$service_account_name@$project.iam.gserviceaccount.com
project_number=$(gcloud projects describe "$project" --format='value(projectNumber)')
pool_path=projects/$project_number/locations/global/workloadIdentityPools/$pool

gcloud services enable iam.googleapis.com iamcredentials.googleapis.com \
    sts.googleapis.com storage.googleapis.com --project "$project"

if ! gcloud storage buckets describe "$bucket" --project "$project" > /dev/null 2>&1; then
    gcloud storage buckets create "$bucket" --project "$project" \
        --location "$location" --uniform-bucket-level-access
fi
gcloud storage buckets update "$bucket" --uniform-bucket-level-access \
    --no-public-access-prevention > /dev/null
gcloud storage buckets add-iam-policy-binding "$bucket" \
    --member=allUsers --role=roles/storage.objectViewer > /dev/null

if ! gcloud iam service-accounts describe "$service_account" --project "$project" > /dev/null 2>&1; then
    gcloud iam service-accounts create "$service_account_name" --project "$project" \
        --display-name "eacp docs publisher (GitHub Actions)"
fi
gcloud storage buckets add-iam-policy-binding "$bucket" \
    --member="serviceAccount:$service_account" --role=roles/storage.objectAdmin > /dev/null

if ! gcloud iam workload-identity-pools describe "$pool" --project "$project" \
        --location global > /dev/null 2>&1; then
    gcloud iam workload-identity-pools create "$pool" --project "$project" \
        --location global --display-name "GitHub Actions"
fi

repository_condition=$(printf "assertion.repository == '%s' || " "${repositories[@]}")
repository_condition=${repository_condition% || }
provider_flags=(--project "$project" --location global --workload-identity-pool "$pool"
    --attribute-mapping "google.subject=assertion.sub,attribute.repository=assertion.repository,attribute.ref=assertion.ref"
    --attribute-condition "$repository_condition")
if gcloud iam workload-identity-pools providers describe "$provider" --project "$project" \
        --location global --workload-identity-pool "$pool" > /dev/null 2>&1; then
    gcloud iam workload-identity-pools providers update-oidc "$provider" "${provider_flags[@]}"
else
    gcloud iam workload-identity-pools providers create-oidc "$provider" "${provider_flags[@]}" \
        --issuer-uri https://token.actions.githubusercontent.com
fi

for repository in "${repositories[@]}"; do
    gcloud iam service-accounts add-iam-policy-binding "$service_account" --project "$project" \
        --role roles/iam.workloadIdentityUser \
        --member "principalSet://iam.googleapis.com/$pool_path/attribute.repository/$repository" \
        > /dev/null
done

echo
echo "Set these as repository variables (Settings > Secrets and variables > Actions > Variables):"
echo "EACP_DOCS_WORKLOAD_IDENTITY_PROVIDER=$pool_path/providers/$provider"
echo "EACP_DOCS_SERVICE_ACCOUNT=$service_account"
